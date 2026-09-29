#include "strength_per_suzerain.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include <ykkz000/civ6/combat.h>
#include <ykkz000/civ6/effect.h>
#include <ykkz000/civ6/unit.h>
#include <ykkz000/extra/player_extra.h>

#include "engine_access.h"

// 每宗主城邦 × Amount 的“单位”战斗力修正。
//
// 缩放发生在引擎的“战斗力修正写入点”（EngineApi::proposedCombatAdjust，即
// GameEffects::ProposedCombat::AdjustPlayerStrengthModifier）。引擎在该处传入它
// 自己解析出的权威 playerId 与目标单位（ProposedCombat+0x00），钩子据此查该单位的
// 侧表条目、统计宗主数并把 amount 换成 strength_per_suzerain × 宗主数；模板的 +0x5C
// 记账仍按未缩放值，由包装层补差。
//
// 侧表落在 PlayerExtras 的 unit_extras（顶层键 = 玩家，子键 = Unit::Instance+0xB0 的
// 单位 id）。每个效果实例以自身指针（self）为键 upsert，聚合值 = 各实例之和，因此
// 同一实例重复 Apply 不会重复累加（幂等），不同实例可叠加，不同单位互不干扰——
// 这正是上一版“每玩家单值被单位数放大（N 变 M×N）”的修正。
//
// 钩子必须无损转发所有非本效果调用（active 窗口为空或 amount == 0 时早退），
// 因为它会被大量内置效果使用。
namespace ykkz000::plugin {
namespace {

// 缩放后写入 Amount 的防御上限：超过即视为解析错误，退回模板原值。
constexpr int kMaxScaledStrengthAmount = 1000000;

using StrengthAccumulateFn =
    void (*)(civ6::GameEffects::ProposedCombat* target, int player_id, int amount);

std::mutex kAccumulateMutex;
StrengthAccumulateFn kAccumulateOriginal = nullptr;
void* kAccumulateTarget = nullptr;
bool kAccumulateInstalled = false;

// 命中日志计数：仅前 16 条与每 4096 条打印一次，避免逐次写入刷屏。
std::atomic<long> kApplyLogCount{0};

// 本线程是否正在执行我们效果对象的模板 Apply/Remove（非空即该 self）；以及本次窗口
// 内累计的 (scaled - original) 与落到写入点的次数。
thread_local void* kApplyActive = nullptr;
thread_local int kScaledDelta = 0;
thread_local int kWriteCount = 0;
// 本次窗口内钩子首次捕获到的权威玩家（顶层侧表键）；未捕获为 -1。
thread_local int kWindowPlayerId = -1;
// 本次窗口是否只捕获玩家与单位、不缩放：Apply=false（钩子按每宗主值缩放），
// Remove=true（模板按已修正的 +0x5C 精确回退，不能再缩放）。
thread_local bool kWindowCaptureOnly = false;
// 本次窗口内钩子首次捕获到的目标单位（指针仅用于日志/一致性判断）与其单位 id
// （子表键）；未捕获为 nullptr/-1。
thread_local void* kWindowUnit = nullptr;
thread_local std::int32_t kWindowUnitId = -1;
// 本次 Apply 窗口内按玩家类型缓存的宗主数：宗主关系在单次 Apply 内不会变化，故可
// 安全复用；窗口开启时置 -1。避免模板对同一玩家多次写入时重复遍历玩家向量。
thread_local int kCountPlayerId = -1;
thread_local int kCountValue = -1;

// 单位侧表子键：Unit::Instance +0xB0（发布镜像 FUN_18005c4a0/FUN_1803a48d0 证据）。
// 读失败或为负视为不可用（返回 -1），此时不维护单位侧表并回退模板 Amount。
std::int32_t UnitIdOf(const void* unit) {
  if (unit == nullptr) {
    return -1;
  }
  const std::int32_t unit_id =
      TryReadOr(unit, &civ6::Unit::Instance::unit_id, std::int32_t{-1});
  return unit_id >= 0 ? unit_id : -1;
}

// 重算聚合值 = Σ 各实例值。upsert/erase 之后必须调用，保证 strength_per_suzerain
// 与 instances 同步；若聚合值超出防御上限，给一次告警（防再次膨胀）。
void RecomputeStrength(extra::UnitExtra& unit) {
  std::int32_t sum = 0;
  for (const auto& entry : unit.instances) {
    sum += entry.second;
  }
  unit.strength_per_suzerain = sum;
  if (sum > kMaxScaledStrengthAmount || sum < -kMaxScaledStrengthAmount) {
    static std::atomic<bool> kLoggedOverflow{false};
    if (!kLoggedOverflow.exchange(true)) {
      LogF(0, "extra: strength_per_suzerain=%d exceeds cap (unit_id=%d); "
              "side table may be inflated",
           sum, unit.unit_id);
    }
  }
}

void StrengthAccumulate_Hook(civ6::GameEffects::ProposedCombat* target, int player_id,
                             int amount) {
  if (kAccumulateOriginal == nullptr) {
    // 已启用但跳板为空：引擎调用会被吞掉。正常状态不会出现；一旦出现必须让其可见，
    // 否则表现为“所有加力只显示不生效”。
    static std::atomic<bool> kLoggedNoTrampoline{false};
    if (!kLoggedNoTrampoline.exchange(true)) {
      Log(0, "strength: detour without trampoline; call dropped");
    }
    return;
  }
  if (kApplyActive == nullptr || amount == 0) { // 非本效果 / 空值：原样转发
    kAccumulateOriginal(target, player_id, amount);
    return;
  }
  ++kWriteCount;
  // 记录本窗口内的权威玩家（同一窗口内多次写入应为同一玩家）。
  if (kWindowPlayerId < 0) {
    kWindowPlayerId = player_id;
  } else if (kWindowPlayerId != player_id) {
    static std::atomic<bool> kLoggedPlayerMismatch{false};
    if (!kLoggedPlayerMismatch.exchange(true)) {
      LogF(0, "strength: window player mismatch (%d != %d)", kWindowPlayerId, player_id);
    }
  }
  // 捕获目标单位与其单位 id（Apply 与 Remove 都要用它维护单位侧表）。
  // 同时一次性交叉校验“目标单位所有者 == playerId”，确认对落账分支的理解无误。
  if (kWindowUnit == nullptr) {
    civ6::Unit::Instance* unit = nullptr;
    (void)TryRead(target, &civ6::GameEffects::ProposedCombat::unit, unit);
    if (unit != nullptr) {
      kWindowUnit = unit;
      kWindowUnitId = UnitIdOf(unit);
      const civ6::PlayerTypes unit_owner =
          TryReadOr(unit, &civ6::Unit::Instance::owner, civ6::kInvalidPlayerType);
      static std::atomic<bool> kLoggedOwnerMismatch{false};
      if (civ6::PlayerTypeIndex(unit_owner) != player_id &&
          !kLoggedOwnerMismatch.exchange(true)) {
        LogF(0, "strength: hook target owner mismatch (playerId=%d unit+0x%zX=%d)",
             player_id, offsetof(civ6::Unit::Instance, owner),
             civ6::PlayerTypeIndex(unit_owner));
      }
    }
  }
  if (kWindowCaptureOnly) {
    // Remove 窗口：仅捕获玩家/单位，不缩放（模板按已修正的 +0x5C 精确回退）。
    kAccumulateOriginal(target, player_id, amount);
    return;
  }
  void* player = PlayerById(player_id); // 优先按 +0xD8 匹配（下标≠玩家类型）
  if (player == nullptr || !IsRealPlayer(player)) {
    player = PlayerAtIndex(player_id);
  }
  int count = -1;
  if (player != nullptr) {
    count = (player_id == kCountPlayerId) ? kCountValue : CountSuzerainsOfPlayer(player);
    if (count >= 0) { // 记录本次 Apply 窗口内的缓存（含 0：0 是合法宗主数）
      kCountPlayerId = player_id;
      kCountValue = count;
    }
  }
  if (count < 0) {
    static std::atomic<bool> kLoggedCountUnresolved{false};
    if (!kLoggedCountUnresolved.exchange(true)) {
      LogF(0, "strength: hook count unresolved (playerId=%d)", player_id);
    }
    kAccumulateOriginal(target, player_id, amount);
    return;
  }
  // 取值优先用单位侧表（权威“每宗主”值 = 各实例之和）；表未建立（首次 Apply 之前）或
  // 模块被禁用时退回模板 Amount，保证行为不变。
  int per_suzerain = amount;
  if (kWindowUnitId >= 0) {
    const std::int32_t stored =
        extra::PlayerExtras().FindUnitStrength(player_id, kWindowUnitId);
    if (stored != 0) {
      per_suzerain = stored;
      if (per_suzerain != amount) { // 多实例聚合的正常情形
        static std::atomic<bool> kLoggedTableDiff{false};
        if (!kLoggedTableDiff.exchange(true)) {
          LogF(2, "strength: extra per-suzerain=%d != amount=%d for player=%d unit=%d",
               per_suzerain, amount, player_id, kWindowUnitId);
        }
      }
    } else {
      static std::atomic<bool> kLoggedFallback{false};
      if (!kLoggedFallback.exchange(true)) {
        LogF(2, "strength: extra per-suzerain missing for player=%d unit=%d; "
               "fall back to amount=%d",
             player_id, kWindowUnitId, amount);
      }
    }
  } else {
    static std::atomic<bool> kLoggedNoUnitId{false};
    if (!kLoggedNoUnitId.exchange(true)) {
      LogF(0, "strength: unit id unavailable (unit=%p); unit side table not used",
           kWindowUnit);
    }
  }
  const long long scaled = static_cast<long long>(per_suzerain) * count;
  if (scaled > kMaxScaledStrengthAmount || scaled < -kMaxScaledStrengthAmount) {
    kAccumulateOriginal(target, player_id, amount);
    return;
  }
  kScaledDelta += static_cast<int>(scaled) - amount;
  const long hit = ++kApplyLogCount;
  if (hit <= 16 || (hit % 4096) == 0) {
    LogF(2, "strength: hook playerId=%d unit=%d amount=%d->%d suzerains=%d", player_id,
         kWindowUnitId, amount, static_cast<int>(scaled), count);
  }
  if ((hit % 4096) == 0) {
    // 一次性规模自检：帮助发现侧表膨胀/键失效。
    static std::atomic<bool> kLoggedStats{false};
    if (!kLoggedStats.exchange(true)) {
      const auto stats = extra::PlayerExtras().CountStats();
      LogF(1, "extra: table players=%zu cities=%zu units=%zu", stats.players,
           stats.cities, stats.units);
    }
  }
  kAccumulateOriginal(target, player_id, static_cast<int>(scaled));
}

// 缩放已由写入点钩子完成，这里只做状态管理：
//   - 打开窗口，让模板按原 Amount 调用（玩家定位、StackPercent/Scalar 等逻辑仍由
//     模板负责）；窗口内钩子捕获权威 playerId 与目标单位并按侧表值缩放；
//   - Apply：窗口关闭后把该实例的 Amount upsert 进目标单位的侧表条目（instances[self]
//     = amount，聚合 = Σ），再把模板 +0x5C 里记的未缩放值补差为缩放后总量（预览
//     {Property} 与 Remove 都依赖它）；
//   - Remove：窗口仅捕获玩家/单位不缩放，把该实例从单位侧表删除（erase(self)）并重算
//     聚合；模板按 +0x5C（Apply 时已修正为缩放后总量）精确回退。
// 幂等：instances 以 self 为键 upsert ⇒ 引擎重放 Apply 不会重复累加。
struct WindowState {
  void* active;
  int delta;
  int writes;
  int count_player;
  int count_value;
  int window_player;
  bool capture_only;
  void* window_unit;
  std::int32_t window_unit_id;
};

// 打开窗口并返回旧值快照，供 CloseWindow 还原（支持嵌套：模板可能在同一线程上触发
// 另一个效果对象的应用）。
WindowState OpenWindow(void* self, bool capture_only) {
  const WindowState previous{kApplyActive,   kScaledDelta,     kWriteCount,
                             kCountPlayerId, kCountValue,      kWindowPlayerId,
                             kWindowCaptureOnly, kWindowUnit, kWindowUnitId};
  kApplyActive = self;
  kScaledDelta = 0;
  kWriteCount = 0;
  kCountPlayerId = -1; // 新窗口：宗主数缓存失效
  kCountValue = -1;
  kWindowPlayerId = -1;
  kWindowCaptureOnly = capture_only;
  kWindowUnit = nullptr;
  kWindowUnitId = -1;
  return previous;
}

void CloseWindow(const WindowState& previous) {
  kApplyActive = previous.active;
  kScaledDelta = previous.delta;
  kWriteCount = previous.writes;
  kCountPlayerId = previous.count_player;
  kCountValue = previous.count_value;
  kWindowPlayerId = previous.window_player;
  kWindowCaptureOnly = previous.capture_only;
  kWindowUnit = previous.window_unit;
  kWindowUnitId = previous.window_unit_id;
}

std::uint64_t ApplyPerSuzerain(void* self, void* a1, void* a2, void* a3, int sign) {
  const bridge::EngineApi* engine = Context().engine;
  if (!IsCandidateObject(self) || engine == nullptr || engine->effectStrengthApply == nullptr ||
      engine->effectStrengthRemove == nullptr) {
    return 0;
  }
  // 与引擎同样的 4 个实参转发；Amount 缩放只发生在 Apply 窗口的写入点钩子里。
  const auto call_template = [&](void* s) -> std::uint64_t {
    return sign < 0
               ? reinterpret_cast<bridge::ApplyFn>(engine->effectStrengthRemove)(s, a1, a2, a3)
               : reinterpret_cast<bridge::ApplyFn>(engine->effectStrengthApply)(s, a1, a2, a3);
  };

  const WindowState previous = OpenWindow(self, sign < 0);
  const std::uint64_t result = call_template(self);
  const int writes = kWriteCount;
  const int delta = kScaledDelta;
  const int window_player = kWindowPlayerId;
  const std::int32_t window_unit_id = kWindowUnitId;
  CloseWindow(previous);

  if (writes == 0) {
    // 窗口内一次都没落到写入点：钩子/入口未生效（先怀疑 DLL 未更新或入口不符）；
    // 也可能是模板 Remove 的 Amount（+0x5C）恰为 0 导致钩子早退。
    static std::atomic<bool> kLoggedHookMiss{false};
    if (!kLoggedHookMiss.exchange(true)) {
      LogF(0, "strength: hook never fired (self=%p sign=%d)", self, sign);
    }
    return result;
  }
  if (window_player < 0) {
    return result;
  }
  // 维护单位侧表：Apply upsert / Remove erase，随后重算聚合（Σ）。
  if (window_unit_id >= 0) {
    std::int32_t amount = 0;
    if (TryRead(self, &civ6::AdjustPlayerStrengthModifier::amount, amount)) {
      if (sign < 0) {
        extra::PlayerExtras().EditUnit(window_player, window_unit_id,
                                       [&](extra::UnitExtra& unit) {
                                         unit.instances.erase(self);
                                         RecomputeStrength(unit);
                                       });
        extra::PlayerExtras().EraseIfEmptyUnit(window_player, window_unit_id);
      } else {
        extra::PlayerExtras().EditUnit(window_player, window_unit_id,
                                       [&](extra::UnitExtra& unit) {
                                         unit.instances[self] = amount;
                                         RecomputeStrength(unit);
                                       });
        static std::atomic<bool> kLoggedFirstExtra{false};
        if (!kLoggedFirstExtra.exchange(true)) {
          LogF(1, "strength: extra per-suzerain=%d for player=%d unit=%d (self=%p)",
               amount, window_player, window_unit_id, self);
        }
      }
    } else {
      static std::atomic<bool> kLoggedNoAmount{false};
      if (!kLoggedNoAmount.exchange(true)) {
        LogF(0, "strength: cannot read effect Amount (self=%p); unit side table "
               "not maintained",
             self);
      }
    }
  }
  if (delta != 0) {
    std::int32_t applied_total = 0;
    if (TryRead(self, &civ6::AdjustPlayerStrengthModifier::applied_total, applied_total)) {
      (void)TryWrite(self, &civ6::AdjustPlayerStrengthModifier::applied_total,
                     applied_total + delta);
    }
  }
  return result;
}

bool InstallHooksOnce() {
  const bridge::Host* host = Context().host;
  const bridge::EngineApi* engine = Context().engine;
  if (host == nullptr || host->installHook == nullptr || host->removeHook == nullptr ||
      engine == nullptr || engine->proposedCombatAdjust == nullptr) {
    Log(0, "strength: combat write point unavailable; effect degrades to the template "
           "amount (no per-suzerain scaling)");
    return false;
  }
  std::lock_guard<std::mutex> guard(kAccumulateMutex);
  // 幂等前置：已安装且跳板可用即视为成功。避免对已启用的 hook 重复 enable，也避免在
  // loader 误报失败时把仍然生效的跳板清空。
  if (kAccumulateInstalled && kAccumulateOriginal != nullptr && kAccumulateTarget != nullptr) {
    return true;
  }
  void* const target = engine->proposedCombatAdjust;
  const int status = host->installHook(host->pluginHandle, target,
                                       reinterpret_cast<void*>(&StrengthAccumulate_Hook),
                                       reinterpret_cast<void**>(&kAccumulateOriginal));
  if (status != 0) {
    LogF(0, "strength: accumulate hook install -> %d", status);
    // 先撤销接管确保 hook 不再拦截，再清空跳板；若顺序反转，会留下“已启用但跳板为空”
    // 的状态，使 detour 吞掉引擎的每一次调用。
    (void)host->removeHook(host->pluginHandle, target);
    kAccumulateOriginal = nullptr;
    kAccumulateInstalled = false;
    kAccumulateTarget = nullptr;
    return false;
  }
  kAccumulateTarget = target;
  kAccumulateInstalled = true;
  LogF(1, "strength: combat write point hook installed target=%p detour=%p trampoline=%p",
       kAccumulateTarget, reinterpret_cast<void*>(&StrengthAccumulate_Hook),
       reinterpret_cast<void*>(kAccumulateOriginal));
  return true;
}

void ResetWindow() {
  kApplyActive = nullptr;
  kScaledDelta = 0;
  kWriteCount = 0;
  kCountPlayerId = -1;
  kCountValue = -1;
  kWindowPlayerId = -1;
  kWindowCaptureOnly = false;
  kWindowUnit = nullptr;
  kWindowUnitId = -1;
}

// 本模块的 hook 安装入口：无捕获 lambda（可作函数指针），状态留在文件作用域供 detour
// 与上下文回调共用。写入点不可用时返回 0：效果退化为模板原值，不因此让整次注册失败。
const bridge::EffectPrepareFn kStrengthPrepare = +[](void* /*userData*/) -> int {
  (void)InstallHooksOnce();
  return 0;
};

// 常驻装配对象：loader 长期持有 impl 指针，不可为临时对象。
bridge::EffectImpl g_impl = {};
bridge::EffectDesc g_desc = {};
bool g_described = false;

// 装配本模块的 EffectImpl/EffectDesc；返回常驻的 desc 供 plugin.cpp 登记。
const bridge::EffectDesc* Describe(const bridge::Host& host) {
  (void)host;
  if (g_described) {
    return &g_desc;
  }
  g_desc.typeName = "EFFECT_YKKZ000_ADJUST_PLAYER_STRENGTH_PER_SUZERAIN_MODIFIER";
  g_desc.templateEffect = "EFFECT_ADJUST_PLAYER_STRENGTH_MODIFIER";
#if !defined(YKKZ000_DISABLE_CUSTOM_BEHAVIOR) && !defined(YKKZ000_DISABLE_STRENGTH_PER_SUZERAIN)
  const bridge::EngineApi* engine = host.engine;
  if (engine == nullptr) {
    return nullptr;
  }
  g_impl.templateApply = engine->effectStrengthApply;
  g_impl.templateRemove = engine->effectStrengthRemove;
  g_impl.apply = &StrengthApply;
  g_impl.remove = &StrengthRemove;
  g_impl.label = "player-strength-per-suzerain";
  g_desc.impl = &g_impl;
  g_desc.prepare = kStrengthPrepare;
#else
  g_desc.impl = nullptr;    // 关闭自定义行为：退化为模板行为
  g_desc.prepare = nullptr; // 不装 hook
#endif
  g_described = true;
  return &g_desc;
}

const EffectModule g_module = {"player-strength-per-suzerain", &Describe, &StrengthOnContext,
                               &StrengthShutdown};

} // namespace

const EffectModule* StrengthModule() { return &g_module; }

void StrengthUninstallHook() {
  const bridge::Host* host = Context().host;
  std::lock_guard<std::mutex> guard(kAccumulateMutex);
  if (kAccumulateInstalled && host != nullptr && host->removeHook != nullptr &&
      kAccumulateTarget != nullptr) {
    (void)host->removeHook(host->pluginHandle, kAccumulateTarget);
    kAccumulateInstalled = false;
  }
  ResetWindow();
  // 停用/卸载：清空共用侧表，避免跨局残留（city-yield 模块亦会清，幂等）。
  extra::PlayerExtras().Clear();
}

void StrengthOnContext(bridge::GameContextEvent event, void* /*context*/) {
  if (event == bridge::GameContextEvent::kCreated) {
    (void)InstallHooksOnce();
    ResetWindow();
    extra::PlayerExtras().Clear(); // 新上下文：旧玩家/城市/单位键全部失效
    return;
  }
  StrengthUninstallHook();
}

void StrengthShutdown() {
  StrengthUninstallHook();
}

std::uint64_t StrengthApply(void* self, void* a1, void* a2, void* a3) {
  return ApplyPerSuzerain(self, a1, a2, a3, 1);
}

std::uint64_t StrengthRemove(void* self, void* a1, void* a2, void* a3) {
  return ApplyPerSuzerain(self, a1, a2, a3, -1);
}

} // namespace ykkz000::plugin
