#include "city_yield_per_population.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <ykkz000/civ6/city.h>
#include <ykkz000/civ6/effect.h>

#include "engine_access.h"

// 每人口 × Amount% 的城市产出修正。
//
// 思路：自定义 EffectType 复用 EFFECT_ADJUST_CITY_YIELD_MODIFIER 的工厂 Create
// （引擎 Initialize 已按模板解析 YieldType/Amount 参数），Loader 再把产出对象的
// Apply/Remove 槽替换为下方实现。Apply 时按“当时人口 × Amount”写入城市产出修正；
// 引擎的 ChangePopulation 只改人口、不重算修饰器，故用人口 hook 按增量补正。
namespace ykkz000::plugin {
namespace {

using ChangeYieldModifierFn = void (*)(void* city, int yield_type, int amount);
using ChangePopulationFn = void (*)(void* city, int delta);

// 已应用效果的“每人口”累计记录：键为城市对象指针，值为该城市上每个
// (YieldType, Amount) 条目及其当前人口基准。Apply 时登记、Remove 时注销；
// 人口变化 hook 据此以 Amount × 人口增量补正，使累计恒等于 Amount × 当前人口。
//
// 指针键仅在同一城市存活期间稳定，因此额外记录城市 ID 以识别指针被回收后复用：
// 若复用地址上的城市 ID 与登记时不一致，则丢弃原条目而不是误加到新城市。
struct AppliedEntry {
  int yield_type = 0;
  int amount = 0;             // 每人口增量（非乘人口后的值）
  int applied_population = 0; // 已应用总量 = Amount × 本值
};

struct AppliedCity {
  int id = -1; // City::Instance 城市 ID；-1 表示尚未登记
  std::vector<AppliedEntry> entries;
};

std::mutex kAppliedMutex;
std::unordered_map<void*, AppliedCity> kAppliedCities;

std::mutex kHookMutex;
ChangePopulationFn kChangePopulationOriginal = nullptr;
void* kChangePopulationTarget = nullptr;
bool kHookInstalled = false;

int ReadCityId(const void* city) {
  return TryReadOr(city, &civ6::City::Instance::city_id, std::int32_t{0});
}

// 校验指针指向的仍是登记时的同一座城市：指针被回收复用后会得到不同的 ID，
// 据此丢弃旧条目，避免把已销毁城市的每人口修正套用到新城市。
bool CityRecordMatches(const void* city, const AppliedCity& record) {
  return ReadCityId(city) == record.id;
}

// 读取效果对象上的 (YieldType, Amount) 条目，与 Effects::AdjustCityYieldModifier
// 的遍历结构一致。
bool ReadEffectEntries(void* self, std::vector<AppliedEntry>& out) {
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
    out.push_back(AppliedEntry{yields[i], amounts[i], 0});
  }
  return !out.empty();
}

void RememberAppliedCityModifier(void* city, int yield_type, int amount,
                                 int applied_population) {
  if (city == nullptr) {
    return;
  }
  const int id = ReadCityId(city);
  std::lock_guard<std::mutex> guard(kAppliedMutex);
  AppliedCity& record = kAppliedCities[city];
  if (record.id != id) {
    // 指针被复用为另一座城市：丢弃旧条目后按新城市登记。
    record.entries.clear();
    record.id = id;
  }
  record.entries.push_back(AppliedEntry{yield_type, amount, applied_population});
}

// 注销一个条目并通过 applied_total 返回其已应用总量（Amount × 人口基准）。
// 若指针已被复用为其它城市，则丢弃该城市的全部旧条目并返回 false。
bool ForgetAppliedCityModifier(void* city, int yield_type, int amount, int& applied_total) {
  applied_total = 0;
  if (city == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> guard(kAppliedMutex);
  const auto it = kAppliedCities.find(city);
  if (it == kAppliedCities.end()) {
    return false;
  }
  if (!CityRecordMatches(city, it->second)) {
    kAppliedCities.erase(it);
    return false;
  }
  auto& entries = it->second.entries;
  for (auto cursor = entries.begin(); cursor != entries.end(); ++cursor) {
    if (cursor->yield_type == yield_type && cursor->amount == amount) {
      applied_total = cursor->amount * cursor->applied_population;
      entries.erase(cursor);
      if (entries.empty()) {
        kAppliedCities.erase(it);
      }
      return true;
    }
  }
  return false;
}

// 人口变化 hook 的补正：把每个已登记条目从各自的人口基准校正到当前人口
// （即追加 Amount × 人口差）。先复制条目再释放锁，避免在调用引擎时持锁。
void AdjustAppliedCityPopulation(void* city) {
  if (city == nullptr) {
    return;
  }
  const int population =
      TryReadOr(city, &civ6::City::Instance::population, std::int32_t{0});
  LogF(2, "city-yield: adjust city=%p pop=%d", city, population);
  if (population < 0 || population > kMaxPlausiblePopulation) {
    LogF(0, "city-yield: implausible population %d (city=%p); skipping write", population, city);
    return;
  }
  std::vector<AppliedEntry> entries;
  {
    std::lock_guard<std::mutex> guard(kAppliedMutex);
    const auto it = kAppliedCities.find(city);
    if (it == kAppliedCities.end()) {
      return;
    }
    if (!CityRecordMatches(city, it->second)) {
      kAppliedCities.erase(it);
      return;
    }
    entries = it->second.entries;
  }
  const bridge::EngineApi* engine = Context().engine;
  if (engine == nullptr || engine->changeYieldModifier == nullptr) {
    return;
  }
  const auto change = reinterpret_cast<ChangeYieldModifierFn>(engine->changeYieldModifier);
  for (const AppliedEntry& entry : entries) {
    const int delta = population - entry.applied_population;
    if (delta != 0) {
      change(city, entry.yield_type, entry.amount * delta);
    }
  }
  std::lock_guard<std::mutex> guard(kAppliedMutex);
  const auto it = kAppliedCities.find(city);
  if (it == kAppliedCities.end() || !CityRecordMatches(city, it->second)) {
    return;
  }
  for (AppliedEntry& entry : it->second.entries) {
    entry.applied_population = population;
  }
}

void ClearAppliedCityModifiers() {
  std::lock_guard<std::mutex> guard(kAppliedMutex);
  kAppliedCities.clear();
}

// 与 Effects::AdjustCityYieldModifier 相同的遍历结构，但把 Amount 乘以城市人口，
// sign 用于 Apply(+1)/Remove(-1)；同时维护累计记录。
// Apply 记录人口基准；Remove 按记录精确回退该条目已应用的总量（即使人口 hook
// 未安装、人口已变化，也不会残留 Amount × 人口差）。
int ApplyPerPopulation(void* self, void* city, int sign) {
  const bridge::EngineApi* engine = Context().engine;
  if (self == nullptr || city == nullptr || engine == nullptr ||
      engine->changeYieldModifier == nullptr) {
    return 0;
  }
  std::vector<AppliedEntry> entries;
  if (!ReadEffectEntries(self, entries)) {
    return 0;
  }
  const auto change = reinterpret_cast<ChangeYieldModifierFn>(engine->changeYieldModifier);
  const int population = TryReadOr(city, &civ6::City::Instance::population, std::int32_t{0});
  if (population < 0 || population > kMaxPlausiblePopulation) {
    LogF(0, "city-yield: implausible population %d (city=%p); skipping write", population, city);
    return 0;
  }
  static std::atomic<bool> kLoggedFirstApply{false};
  if (!kLoggedFirstApply.exchange(true)) {
    LogF(1, "city-yield: first apply self=%p city=%p pop=%d entries=%zu", self, city,
         population, entries.size());
    for (const AppliedEntry& entry : entries) {
      LogF(2, "city-yield: entry yield=%d amount=%d", entry.yield_type, entry.amount);
    }
  }
  for (const AppliedEntry& entry : entries) {
    if (sign > 0) {
      change(city, entry.yield_type, entry.amount * population);
      RememberAppliedCityModifier(city, entry.yield_type, entry.amount, population);
      continue;
    }
    int applied_total = 0;
    const int revert = ForgetAppliedCityModifier(city, entry.yield_type, entry.amount,
                                                 applied_total)
                           ? applied_total
                           : entry.amount * population;
    change(city, entry.yield_type, -revert);
  }
  return 1;
}

// ChangePopulation 自带 `0 < population + delta` 的下限保护，可能不按 delta 生效；
// 因此以“调用前后的人口差”判断是否发生实际变化，再由 AdjustAppliedCityPopulation
// 按各条目自身的人口基准校正到当前人口。
void ChangePopulation_Hook(void* city, int delta) {
  if (kChangePopulationOriginal == nullptr) {
    // 已启用但跳板为空：引擎调用会被吞掉，导致本会话人口变化静默丢失。
    static std::atomic<bool> kLoggedNoTrampoline{false};
    if (!kLoggedNoTrampoline.exchange(true)) {
      Log(0, "city-yield: detour without trampoline; call dropped");
    }
    return;
  }
  if (city == nullptr) {
    kChangePopulationOriginal(city, delta);
    return;
  }
  static std::atomic<long> kCalls{0};
  const long call = ++kCalls;
  const bool verbose = call <= 32 || (call % 1024) == 0;
  const int before = TryReadOr(city, &civ6::City::Instance::population, std::int32_t{0});
  if (verbose) {
    LogF(1, "city-yield hook: call#%ld city=%p delta=%d pop=%d", call, city, delta, before);
  }
  kChangePopulationOriginal(city, delta);
  if (TryReadOr(city, &civ6::City::Instance::population, std::int32_t{0}) != before) {
    AdjustAppliedCityPopulation(city);
  }
}

bool InstallHooksOnce() {
  const bridge::Host* host = Context().host;
  const bridge::EngineApi* engine = Context().engine;
  if (host == nullptr || host->installHook == nullptr || host->removeHook == nullptr ||
      engine == nullptr || engine->changePopulation == nullptr) {
    Log(0, "city-yield: ChangePopulation entry unavailable; effects will fall back to a "
           "founding-time population snapshot");
    return false;
  }
  std::lock_guard<std::mutex> guard(kHookMutex);
  // 幂等前置：已安装且跳板可用即视为成功。避免对已启用的 hook 重复 enable，也避免在
  // loader 误报失败时把仍然生效的跳板清空。
  if (kHookInstalled && kChangePopulationOriginal != nullptr &&
      kChangePopulationTarget != nullptr) {
    return true;
  }
  void* const target = engine->changePopulation;
  const int status = host->installHook(host->pluginHandle, target,
                                       reinterpret_cast<void*>(&ChangePopulation_Hook),
                                       reinterpret_cast<void**>(&kChangePopulationOriginal));
  if (status != 0) {
    LogF(0, "city-yield: population hook install -> %d", status);
    // 先撤销接管确保 hook 不再拦截，再清空跳板；顺序反转会留下“已启用但跳板为空”的
    // 状态，使 detour 吞掉引擎的每一次人口变化调用。
    (void)host->removeHook(host->pluginHandle, target);
    kChangePopulationOriginal = nullptr;
    kHookInstalled = false;
    kChangePopulationTarget = nullptr;
    return false;
  }
  kChangePopulationTarget = target;
  kHookInstalled = true;
  LogF(1,
       "city-yield: population delta compensation installed target=%p detour=%p trampoline=%p",
       kChangePopulationTarget, reinterpret_cast<void*>(&ChangePopulation_Hook),
       reinterpret_cast<void*>(kChangePopulationOriginal));
  return true;
}

// 本模块的 hook 安装入口：无捕获 lambda（可作函数指针），状态留在文件作用域供 detour
// 与上下文回调共用。hook 入口不可用时返回 0：效果优雅降级为“建立时人口快照”，不因此
// 让整次注册失败。
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
  if (kHookInstalled && host != nullptr && host->removeHook != nullptr &&
      kChangePopulationTarget != nullptr) {
    (void)host->removeHook(host->pluginHandle, kChangePopulationTarget);
    kHookInstalled = false;
  }
  ClearAppliedCityModifiers();
}

void CityYieldOnContext(bridge::GameContextEvent event, void* /*context*/) {
  if (event == bridge::GameContextEvent::kCreated) {
    (void)InstallHooksOnce();
    ClearAppliedCityModifiers(); // 新上下文：旧城市指针全部失效
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
  return static_cast<std::uint64_t>(ApplyPerPopulation(self, a1, 1));
}

std::uint64_t CityYieldRemove(void* self, void* a1, void* a2, void* a3) {
  (void)a2;
  (void)a3;
  return static_cast<std::uint64_t>(ApplyPerPopulation(self, a1, -1));
}

} // namespace ykkz000::plugin
