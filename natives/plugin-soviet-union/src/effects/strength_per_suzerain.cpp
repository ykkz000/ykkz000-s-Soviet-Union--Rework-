#include "strength_per_suzerain.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include <ykkz000/civ6/combat.h>
#include <ykkz000/civ6/effect.h>
#include <ykkz000/civ6/unit.h>

#include "engine_access.h"

// 每宗主城邦 × Amount 的玩家单位战斗力修正。
//
// 缩放发生在引擎的“战斗力修正写入点”（EngineApi::proposedCombatAdjust，即
// GameEffects::ProposedCombat::AdjustPlayerStrengthModifier）。引擎在该处传入它
// 自己解析出的权威 playerId，钩子据此查玩家、统计宗主数并把 amount 换成
// amount × 宗主数；模板的 +0x5C 记账仍按未缩放值，由包装层补差。
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

// 本线程是否正在执行我们效果对象的模板 Apply（非空即该 self）；以及本次 Apply 窗口
// 内累计的 (scaled - original) 与落到写入点的次数。
thread_local void* kApplyActive = nullptr;
thread_local int kScaledDelta = 0;
thread_local int kWriteCount = 0;
// 本次 Apply 窗口内按玩家类型缓存的宗主数：宗主关系在单次 Apply 内不会变化，故可
// 安全复用；窗口开启时置 -1。避免模板对同一玩家多次写入时重复遍历玩家向量。
thread_local int kCountPlayerId = -1;
thread_local int kCountValue = -1;

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
  const long long scaled = static_cast<long long>(amount) * count;
  if (scaled > kMaxScaledStrengthAmount || scaled < -kMaxScaledStrengthAmount) {
    kAccumulateOriginal(target, player_id, amount);
    return;
  }
  // 一次性交叉校验“目标单位所有者 == playerId”，确认对落账分支的理解无误。
  civ6::Unit::Instance* target_unit = nullptr;
  (void)TryRead(target, &civ6::GameEffects::ProposedCombat::unit, target_unit);
  if (target_unit != nullptr) {
    const civ6::PlayerTypes unit_owner =
        TryReadOr(target_unit, &civ6::Unit::Instance::owner, civ6::kInvalidPlayerType);
    static std::atomic<bool> kLoggedOwnerMismatch{false};
    if (civ6::PlayerTypeIndex(unit_owner) != player_id &&
        !kLoggedOwnerMismatch.exchange(true)) {
      LogF(0, "strength: hook target owner mismatch (playerId=%d unit+0x%zX=%d)", player_id,
           offsetof(civ6::Unit::Instance, owner), civ6::PlayerTypeIndex(unit_owner));
    }
  }
  kScaledDelta += static_cast<int>(scaled) - amount;
  LogF(1, "strength: hook playerId=%d amount=%d->%d suzerains=%d", player_id, amount,
       static_cast<int>(scaled), count);
  kAccumulateOriginal(target, player_id, static_cast<int>(scaled));
}

// 每宗主城邦 × Amount 的战斗力修正。缩放已由写入点钩子完成，这里只做状态管理：
// Apply 时打开 active 窗口并让模板按原 Amount 调用（玩家定位、StackPercent/Scalar
// 等逻辑仍由模板负责），返回后把模板 +0x5C 里记的未缩放值补差为缩放后总量（预览
// {Property} 与 Remove 都依赖它）。Remove 直接转发模板——模板按 +0x5C 精确回退，
// 而该值已被修正为缩放后总量，故无需再缩放或额外记录。
std::uint64_t ApplyPerSuzerain(void* self, void* a1, void* a2, void* a3, int sign) {
  const bridge::EngineApi* engine = Context().engine;
  if (!IsCandidateObject(self) || engine == nullptr || engine->effectStrengthApply == nullptr ||
      engine->effectStrengthRemove == nullptr) {
    return 0;
  }
  // 与引擎同样的 4 个实参转发；移除路径不对 Amount 缩放（模板按自身记录回退）。
  const auto call_template = [&](void* s) -> std::uint64_t {
    return sign < 0
               ? reinterpret_cast<bridge::ApplyFn>(engine->effectStrengthRemove)(s, a1, a2, a3)
               : reinterpret_cast<bridge::ApplyFn>(engine->effectStrengthApply)(s, a1, a2, a3);
  };

  if (sign < 0) {
    return call_template(self);
  }

  // 打开 active 窗口：窗口内模板对写入点的调用会被钩子按宗主数缩放。保存旧值以
  // 支持嵌套（模板可能在同一线程上触发另一个效果对象的 Apply）。
  void* const previous_active = kApplyActive;
  const int previous_delta = kScaledDelta;
  const int previous_writes = kWriteCount;
  const int previous_count_player = kCountPlayerId;
  const int previous_count_value = kCountValue;
  kApplyActive = self;
  kScaledDelta = 0;
  kWriteCount = 0;
  kCountPlayerId = -1; // 新窗口：宗主数缓存失效
  kCountValue = -1;

  const std::uint64_t result = call_template(self);

  const int writes = kWriteCount;
  const int delta = kScaledDelta;

  kApplyActive = previous_active;
  kScaledDelta = previous_delta;
  kWriteCount = previous_writes;
  kCountPlayerId = previous_count_player;
  kCountValue = previous_count_value;

  if (writes == 0) {
    // 窗口内一次都没落到写入点：钩子/入口未生效（先怀疑 DLL 未更新或入口不符）。
    static std::atomic<bool> kLoggedHookMiss{false};
    if (!kLoggedHookMiss.exchange(true)) {
      LogF(0, "strength: hook never fired (self=%p)", self);
    }
    return result;
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
}

void StrengthOnContext(bridge::GameContextEvent event, void* /*context*/) {
  if (event == bridge::GameContextEvent::kCreated) {
    (void)InstallHooksOnce();
    ResetWindow();
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
