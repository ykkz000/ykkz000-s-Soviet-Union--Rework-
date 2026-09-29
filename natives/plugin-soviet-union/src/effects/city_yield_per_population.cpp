#include "city_yield_per_population.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <vector>

#include <ykkz000/civ6/city.h>
#include <ykkz000/civ6/effect.h>
#include <ykkz000/civ6/tracked_value.h>
#include <ykkz000/extra/city_extra.h>

#include "engine_access.h"

// 每市民 × Amount% 的城市产出修正（读取时乘）。
//
// 思路：Apply/Remove 只把“每市民百分比”聚合进 CityExtra 侧表（键 = city_id + owner），
// 真正的“乘人口”发生在引擎的产出读取路径 City::Instance::CalculateYield 上——
// 返回前向本次返回对象的修正子对象追加 (percent[yield] * population) >> 8。
// 因此人口变化、产出重算天然使用最新人口，无需 ChangePopulation hook，也无
// “已应用人口基准”记账；同参数重复实例只是对称的 += / -=，无歧义、无漂移。
//
// 注入点（发布构建 RVA 0x12FF20）：
//   TrackedValue* City::Instance::CalculateYield(City* this, TrackedValue* out,
//                                                int yield, int typeHash, bool flag)
//   out 为 sret（函数写它并以 RAX 返回）：基础累计 out+0x10；修正子对象 out+0x30，
//   其累计值 out+0x40（FixedPoint<8>，1.0 == +1%，最终产出 = base * (1 + modifier / 25600)）。
namespace ykkz000::plugin {
namespace {

// 效果对象上的一个 (YieldType, Amount) 条目（Amount 为“每市民百分比”整数）。
struct EffectEntry {
  int yield_type = 0;
  int amount = 0;
};

// percent[] 的编码：Amount(%) × 0x10000 ⇒ 0x10000 == +1%/市民。
// 读取时 (percent * population) >> 8 恰好得到 FixedPoint<8> 修正单位（1.0 == +1%）。
constexpr std::int32_t kPercentUnit = 0x10000;

// 单条 Amount 的合理上限（防御解析错误导致的异常值）。
constexpr int kMaxPlausibleAmount = 100000;

// CalculateYield 返回对象（TrackedValue）中修正累计值的偏移。
constexpr std::size_t kModifierAccumulatedOffset =
    offsetof(civ6::TrackedValue, modifier_accumulated);

using CalculateYieldFn = void* (*)(void* city, void* out, int yield, int type_hash,
                                   bool flag);

std::mutex kHookMutex;
CalculateYieldFn kCalculateYieldOriginal = nullptr;
void* kCalculateYieldTarget = nullptr;
bool kCalculateYieldInstalled = false;

// 侧表写代际：Apply/Remove 后自增，令 TLS 缓存失效（写路径在独占锁内完成）。
std::atomic<std::uint64_t> kTableGeneration{0};

// 读路径 TLS 缓存：同键的连续查询直接命中，避免每次加共享锁查表。
// 以侧表键（而非城市指针）为缓存标识：城市指针被回收复用时键不同，天然失效。
struct TlsCache {
  std::uint64_t generation = 0;
  extra::CityKey key{};
  extra::CityExtra extra{};
  bool valid = false;
};
thread_local TlsCache kCache;

// 运行期 dry-run（只记录不修改）：由环境变量 YKKZ000_CITY_YIELD_DRY_RUN 开启，
// 用于上线前按实测值域确认 TrackedValue 布局。默认关闭。
bool DryRunEnabled() {
  static const bool enabled = []() {
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, "YKKZ000_CITY_YIELD_DRY_RUN") != 0 || value == nullptr) {
      return false;
    }
    const bool on = value[0] != '\0' && value[0] != '0';
    std::free(value);
    return on;
  }();
  return enabled;
}

// 读取效果对象上的 (YieldType, Amount) 条目，与 Effects::AdjustCityYieldModifier
// 的遍历结构一致。
bool ReadEffectEntries(void* self, std::vector<EffectEntry>& out) {
  const int entry_count =
      TryReadOr(self, &civ6::CityYieldModifierEffect::entry_count, std::int32_t{0});
  const int amount_count =
      TryReadOr(self, &civ6::CityYieldModifierEffect::amount_count, std::int32_t{0});
  int* yields = nullptr;
  int* amounts = nullptr;
  (void)TryRead(self, &civ6::CityYieldModifierEffect::yields, yields);
  (void)TryRead(self, &civ6::CityYieldModifierEffect::amounts, amounts);
  if (entry_count <= 0 || entry_count > kMaxEffectEntries || amount_count <= 0 ||
      amount_count > kMaxEffectEntries || yields == nullptr || amounts == nullptr) {
    return false;
  }
  const int count = entry_count < amount_count ? entry_count : amount_count;
  out.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    out.push_back(EffectEntry{yields[i], amounts[i]});
  }
  return !out.empty();
}

// 由城市实例取侧表键（读 +0xA8 / +0xD8，做合理性校验）。
bool KeyOf(const void* city, extra::CityKey& key) {
  const std::int32_t city_id =
      TryReadOr(city, &civ6::City::Instance::city_id, std::int32_t{-1});
  const std::int32_t owner = TryReadOr(city, &civ6::City::Instance::owner, std::int32_t{-1});
  if (city_id < 0 || owner < 0 || owner > kMaxPlausiblePlayerIndex) {
    return false;
  }
  key.city_id = city_id;
  key.owner_id = owner;
  return true;
}

// 把条目按 sign(±1) 聚合进侧表：percent += sign * Amount * kPercentUnit。
void ApplyEntries(void* self, void* city, int sign) {
  if (self == nullptr || city == nullptr) {
    return;
  }
  std::vector<EffectEntry> entries;
  if (!ReadEffectEntries(self, entries)) {
    return;
  }
  extra::CityKey key;
  if (!KeyOf(city, key)) {
    return;
  }
  static std::atomic<bool> kLoggedFirstApply{false};
  if (!kLoggedFirstApply.exchange(true)) {
    LogF(1, "city-yield: first apply self=%p city=%p owner=%d entries=%zu", self, city,
         key.owner_id, entries.size());
    for (const EffectEntry& entry : entries) {
      LogF(2, "city-yield: entry yield=%d amount=%d", entry.yield_type, entry.amount);
    }
  }
  constexpr std::int64_t kPercentMin = std::numeric_limits<std::int32_t>::min();
  constexpr std::int64_t kPercentMax = std::numeric_limits<std::int32_t>::max();
  extra::CityExtras().Edit(key, [&](extra::CityExtra& extra) {
    for (const EffectEntry& entry : entries) {
      if (entry.yield_type < 0 ||
          entry.yield_type >= static_cast<int>(civ6::kMaxYields)) {
        continue;
      }
      if (entry.amount < -kMaxPlausibleAmount || entry.amount > kMaxPlausibleAmount) {
        continue;
      }
      const std::int64_t delta =
          static_cast<std::int64_t>(entry.amount) * kPercentUnit * sign;
      std::int64_t updated =
          static_cast<std::int64_t>(extra.percent[entry.yield_type]) + delta;
      if (updated > kPercentMax) {
        updated = kPercentMax;
      } else if (updated < kPercentMin) {
        updated = kPercentMin;
      }
      extra.percent[entry.yield_type] = static_cast<std::int32_t>(updated);
      if (entry.yield_type + 1 > extra.yield_count) {
        extra.yield_count = entry.yield_type + 1;
      }
    }
  });
  if (sign < 0) {
    extra::CityExtras().EraseIfEmpty(key);
  }
  kTableGeneration.fetch_add(1, std::memory_order_release);
}

// 读路径命中查询：优先 TLS 缓存，未命中再查表并按需回填。
const extra::CityExtra* LookupExtra(const extra::CityKey& key) {
  const std::uint64_t generation = kTableGeneration.load(std::memory_order_acquire);
  if (kCache.valid && kCache.generation == generation &&
      extra::CityKeyEqual{}(kCache.key, key)) {
    return &kCache.extra;
  }
  extra::CityExtra found;
  if (!extra::CityExtras().Find(key, found)) {
    kCache.valid = false;
    return nullptr;
  }
  kCache.generation = generation;
  kCache.key = key;
  kCache.extra = found;
  kCache.valid = true;
  return &kCache.extra;
}

// 命中计数：首调用记录值域自检；长期 0 命中给一次 level-0 告警（避免静默失效）。
std::atomic<long> kCallCount{0};
std::atomic<long> kHitCount{0};

void LogFirstCall(void* out, void* city, int yield, int population, std::int32_t base,
                  std::int32_t modifier) {
  static std::atomic<bool> kLogged{false};
  if (kLogged.exchange(true)) {
    return;
  }
  LogF(1,
       "city-yield: first CalculateYield out=%p city=%p yield=%d pop=%d base(+0x10)=%d "
       "modifier(+0x40)=%d",
       out, city, yield, population, base, modifier);
}

void* CalculateYield_Hook(void* city, void* out, int yield, int type_hash, bool flag) {
  if (kCalculateYieldOriginal == nullptr) {
    // 已启用但跳板为空：引擎产出读取会被吞掉。正常不应出现；一旦出现必须可见。
    static std::atomic<bool> kLoggedNoTrampoline{false};
    if (!kLoggedNoTrampoline.exchange(true)) {
      Log(0, "city-yield: detour without trampoline; call dropped");
    }
    return out;
  }
  void* returned = kCalculateYieldOriginal(city, out, yield, type_hash, flag);
  if (city == nullptr || returned == nullptr ||
      yield < 0 || yield >= static_cast<int>(civ6::kMaxYields)) {
    return returned;
  }

  const long call = ++kCallCount;
  if (call == 1) {
    // 首调用 dry-run 自检：只记录返回对象 +0x10/+0x40 的值域与 yield/人口，便于按实测
    // 比对确认 TrackedValue 布局（是否要修改由 DryRunEnabled 决定）。
    std::int32_t first_base = 0;
    std::int32_t first_modifier = 0;
    (void)TryReadAt(returned, std::size_t{0x10}, first_base);
    (void)TryReadAt(returned, kModifierAccumulatedOffset, first_modifier);
    const std::int32_t first_population =
        TryReadOr(city, &civ6::City::Instance::population, std::int32_t{-1});
    LogFirstCall(returned, city, yield, first_population, first_base, first_modifier);
  }
  // 若已有 Apply（侧表被写过）却始终未命中，说明注入点或键不成立，给一次告警。
  if (call >= 8192 && kHitCount.load(std::memory_order_relaxed) == 0 &&
      kTableGeneration.load(std::memory_order_relaxed) != 0) {
    static std::atomic<bool> kLoggedNeverHit{false};
    if (!kLoggedNeverHit.exchange(true)) {
      Log(0, "city-yield: CalculateYield hook fired but never applied (8192 calls); "
             "side table or key may be wrong");
    }
  }
  extra::CityKey key;
  if (!KeyOf(city, key)) {
    return returned;
  }
  const extra::CityExtra* extra = LookupExtra(key);
  if (extra == nullptr || yield >= extra->yield_count) {
    return returned;
  }
  const std::int32_t percent = extra->percent[yield];
  if (percent == 0) {
    return returned;
  }

  const std::int32_t population =
      TryReadOr(city, &civ6::City::Instance::population, std::int32_t{-1});
  if (population < 0 || population > kMaxPlausiblePopulation) {
    static std::atomic<bool> kLoggedBadPopulation{false};
    if (!kLoggedBadPopulation.exchange(true)) {
      LogF(0, "city-yield: implausible population %d (city=%p); skipping write",
           population, city);
    }
    return returned;
  }

  const std::int64_t delta = (static_cast<std::int64_t>(percent) * population) >> 8;
  std::int32_t modifier = 0;
  const bool readable = TryReadAt(returned, kModifierAccumulatedOffset, modifier);
  if (!readable) {
    return returned;
  }
  if (delta == 0) {
    return returned;
  }
  const long hit = ++kHitCount;
  if (hit <= 16 || (hit % 4096) == 0) {
    LogF(1, "city-yield: +%lld modifier (yield=%d pop=%d city=%p owner=%d hit#%ld)",
         static_cast<long long>(delta), yield, population, city, key.owner_id, hit);
  }
  if (DryRunEnabled()) {
    static std::atomic<bool> kLoggedDryRun{false};
    if (!kLoggedDryRun.exchange(true)) {
      LogF(1, "city-yield: dry-run enabled; would add %lld to +0x40 (was %d)",
           static_cast<long long>(delta), modifier);
    }
    return returned;
  }
  const std::int64_t updated = static_cast<std::int64_t>(modifier) + delta;
  if (updated < std::numeric_limits<std::int32_t>::min() ||
      updated > std::numeric_limits<std::int32_t>::max()) {
    return returned;
  }
  (void)TryWriteAt(returned, kModifierAccumulatedOffset,
                   static_cast<std::int32_t>(updated));
  return returned;
}

bool InstallHooksOnce() {
  const bridge::Host* host = Context().host;
  const bridge::EngineApi* engine = Context().engine;
  if (host == nullptr || host->installHook == nullptr || host->removeHook == nullptr ||
      engine == nullptr || engine->cityCalculateYield == nullptr) {
    Log(0, "city-yield: CalculateYield entry unavailable; per-population modifier "
           "will not apply");
    return false;
  }
  std::lock_guard<std::mutex> guard(kHookMutex);
  // 幂等前置：已安装且跳板可用即视为成功。避免对已启用的 hook 重复 enable，也避免在
  // loader 误报失败时把仍然生效的跳板清空。
  if (kCalculateYieldInstalled && kCalculateYieldOriginal != nullptr &&
      kCalculateYieldTarget != nullptr) {
    return true;
  }
  void* const target = engine->cityCalculateYield;
  const int status = host->installHook(host->pluginHandle, target,
                                       reinterpret_cast<void*>(&CalculateYield_Hook),
                                       reinterpret_cast<void**>(&kCalculateYieldOriginal));
  if (status != 0) {
    LogF(0, "city-yield: CalculateYield hook install -> %d", status);
    // 先撤销接管确保 hook 不再拦截，再清空跳板；顺序反转会留下“已启用但跳板为空”的
    // 状态，使 detour 吞掉引擎的每一次产出读取调用。
    (void)host->removeHook(host->pluginHandle, target);
    kCalculateYieldOriginal = nullptr;
    kCalculateYieldInstalled = false;
    kCalculateYieldTarget = nullptr;
    return false;
  }
  kCalculateYieldTarget = target;
  kCalculateYieldInstalled = true;
  LogF(1, "city-yield: CalculateYield hook installed target=%p detour=%p trampoline=%p",
       kCalculateYieldTarget, reinterpret_cast<void*>(&CalculateYield_Hook),
       reinterpret_cast<void*>(kCalculateYieldOriginal));
  return true;
}

void ResetCaches() {
  kCache = TlsCache{};
}

// 清空侧表并令所有线程的 TLS 缓存失效（代际自增对所有线程可见）。
void ClearExtras() {
  extra::CityExtras().Clear();
  kTableGeneration.fetch_add(1, std::memory_order_release);
  ResetCaches();
}

// 本模块的 hook 安装入口：无捕获 lambda（可作函数指针），状态留在文件作用域供 detour
// 与上下文回调共用。入口不可用时返回 0：效果退化为不缩放，不因此让整次注册失败。
const bridge::EffectPrepareFn kCityYieldPrepare = +[](void* /*userData*/) -> int {
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
  g_desc.typeName = "EFFECT_YKKZ000_ADJUST_CITY_YIELD_PER_POPULATION_MODIFIER";
  g_desc.templateEffect = "EFFECT_ADJUST_CITY_YIELD_MODIFIER";
#if !defined(YKKZ000_DISABLE_CUSTOM_BEHAVIOR)
  const bridge::EngineApi* engine = host.engine;
  if (engine == nullptr) {
    return nullptr;
  }
  g_impl.templateApply = engine->effectApply;
  g_impl.templateRemove = engine->effectRemove;
  g_impl.apply = &CityYieldApply;
  g_impl.remove = &CityYieldRemove;
  g_impl.label = "city-yield-per-population";
  g_desc.impl = &g_impl;
  g_desc.prepare = kCityYieldPrepare;
#else
  g_desc.impl = nullptr;    // 关闭自定义行为：退化为模板行为
  g_desc.prepare = nullptr; // 不装 hook
#endif
  g_described = true;
  return &g_desc;
}

const EffectModule g_module = {"city-yield-per-population", &Describe, &CityYieldOnContext,
                               &CityYieldShutdown};

} // namespace

const EffectModule* CityYieldModule() { return &g_module; }

void CityYieldUninstallHook() {
  const bridge::Host* host = Context().host;
  std::lock_guard<std::mutex> guard(kHookMutex);
  if (kCalculateYieldInstalled && host != nullptr && host->removeHook != nullptr &&
      kCalculateYieldTarget != nullptr) {
    (void)host->removeHook(host->pluginHandle, kCalculateYieldTarget);
    kCalculateYieldInstalled = false;
  }
  ClearExtras();
}

void CityYieldOnContext(bridge::GameContextEvent event, void* /*context*/) {
  if (event == bridge::GameContextEvent::kCreated) {
    (void)InstallHooksOnce();
    ClearExtras(); // 新上下文：旧城市键全部失效
    return;
  }
  CityYieldUninstallHook();
}

void CityYieldShutdown() {
  CityYieldUninstallHook();
}

std::uint64_t CityYieldApply(void* self, void* a1, void* a2, void* a3) {
  (void)a2;
  (void)a3;
  ApplyEntries(self, a1, 1);
  return static_cast<std::uint64_t>(1);
}

std::uint64_t CityYieldRemove(void* self, void* a1, void* a2, void* a3) {
  (void)a2;
  (void)a3;
  ApplyEntries(self, a1, -1);
  return static_cast<std::uint64_t>(1);
}

} // namespace ykkz000::plugin
