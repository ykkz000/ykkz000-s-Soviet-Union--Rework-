#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "loader_internal.h"

// 自定义效果对象的行为实现。
//
// 思路：自定义 EffectType 复用模板（如 EFFECT_ADJUST_CITY_YIELD_MODIFIER）的工厂
// Create（引擎 Initialize 已按模板解析 YieldType/Amount 参数），随后把产出对象的
// vtable 克隆一份并按函数指针把 Apply/Remove 槽替换为下方实现，最后把克隆 vtable
// 安装到该实例上。绝不修改引擎共享的静态 vtable。
namespace ykkz000::loader {
namespace {

// MSVC x64 隐藏返回：shared_ptr 返回经由 RDX 传入，函数需在 RAX 回传同一指针。
using FactoryCreateFn = void* (*)(void* self, void* outSharedPtr, const void* params);
using ChangeYieldModifierFn = void (*)(void* city, int yieldType, int amount);

struct BehaviorRecord {
  bridge::EffectBehavior behavior = bridge::EffectBehavior::kInherit;
  void* originalCreate = nullptr;
};

// 注册发生在插件加载时，查询发生在引擎创建效果对象时，可能分属不同线程。
std::mutex g_behaviorMutex;
std::unordered_map<std::uint32_t, BehaviorRecord> g_behaviors;

// 已应用效果的“每人口”累计记录：键为城市对象指针，值为该城市上每个
// (YieldType, Amount) 条目及其当前人口基准。Apply 时登记、Remove 时注销；
// 人口变化 hook 据此以 Amount × 人口增量补正，使累计恒等于 Amount × 当前人口。
//
// 指针键仅在同一城市存活期间稳定，因此额外记录城市 ID 以识别指针被回收后复用：
// 若复用地址上的城市 ID 与登记时不一致，则丢弃原条目而不是误加到新城市。
struct AppliedEntry {
  int yieldType = 0;
  int amount = 0;            // 每人口增量（非乘人口后的值）
  int appliedPopulation = 0; // 该条目累计修正对应的人口（Amount × 本值 = 已应用总量）
};

struct AppliedCity {
  int id = -1; // City::Instance ID（kCityIdOffset）；-1 表示尚未登记
  std::vector<AppliedEntry> entries;
};

std::mutex g_appliedMutex;
std::unordered_map<void*, AppliedCity> g_appliedCities;

bool findBehavior(std::uint32_t typeHash, BehaviorRecord& out) {
  std::lock_guard<std::mutex> guard(g_behaviorMutex);
  const auto it = g_behaviors.find(typeHash);
  if (it == g_behaviors.end()) {
    return false;
  }
  out = it->second;
  return true;
}

template <typename T>
T readField(const void* base, std::size_t offset) {
  T value = {};
  std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(T));
  return value;
}

int readCityId(const void* city) {
  return readField<int>(city, kCityIdOffset);
}

// 校验指针指向的仍是登记时的同一座城市：指针被回收复用后会得到不同的 ID，
// 据此丢弃旧条目，避免把已销毁城市的每人口修正套用到新城市。
bool cityRecordMatches(const void* city, const AppliedCity& record) {
  return readCityId(city) == record.id;
}

// 读取效果对象上的 (YieldType, Amount) 条目，与 Effects::AdjustCityYieldModifier
// 的遍历结构一致。
bool readEffectEntries(void* self, std::vector<AppliedEntry>& out) {
  const int entryCount = readField<int>(self, kEffectEntryCountOffset);
  const int amountCount = readField<int>(self, kEffectAmountCountOffset);
  const int* const yields = readField<const int*>(self, kEffectYieldTypeArrayOffset);
  const int* const amounts = readField<const int*>(self, kEffectAmountArrayOffset);
  if (entryCount <= 0 || yields == nullptr || amounts == nullptr) {
    return false;
  }
  const int count = entryCount < amountCount ? entryCount : amountCount;
  out.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    out.push_back(AppliedEntry{yields[i], amounts[i]});
  }
  return !out.empty();
}

// 与 Effects::AdjustCityYieldModifier 相同的遍历结构，但把 Amount 乘以城市人口，
// sign 用于 Apply(+1)/Remove(-1)；同时维护累计记录。
// Apply 记录人口基准；Remove 按记录精确回退该条目已应用的总量（即使人口 hook
// 未安装、人口已变化，也不会残留 Amount × 人口差）。
int applyPerPopulation(void* self, void* city, int sign) {
  const GameCoreApi& api = gameCore();
  if (self == nullptr || city == nullptr || api.changeYieldModifier == nullptr) {
    return 0;
  }
  std::vector<AppliedEntry> entries;
  if (!readEffectEntries(self, entries)) {
    return 0;
  }
  const auto change = reinterpret_cast<ChangeYieldModifierFn>(api.changeYieldModifier);
  const int population = readField<int>(city, kCityPopulationOffset);
  for (const AppliedEntry& entry : entries) {
    if (sign > 0) {
      change(city, entry.yieldType, entry.amount * population);
      rememberAppliedCityModifier(city, entry.yieldType, entry.amount, population);
      continue;
    }
    int appliedTotal = 0;
    const int revert = forgetAppliedCityModifier(city, entry.yieldType, entry.amount, appliedTotal)
                           ? appliedTotal
                           : entry.amount * population;
    change(city, entry.yieldType, -revert);
  }
  return 1;
}

} // namespace

int registerEffectBehavior(std::uint32_t typeHash, bridge::EffectBehavior behavior,
                           void* originalCreate) {
  std::lock_guard<std::mutex> guard(g_behaviorMutex);
  g_behaviors.insert_or_assign(typeHash, BehaviorRecord{behavior, originalCreate});
  return 0;
}

void rememberAppliedCityModifier(void* city, int yieldType, int amount, int appliedPopulation) {
  if (city == nullptr) {
    return;
  }
  const int id = readCityId(city);
  std::lock_guard<std::mutex> guard(g_appliedMutex);
  AppliedCity& record = g_appliedCities[city];
  if (record.id != id) {
    // 指针被复用为另一座城市：丢弃旧条目后按新城市登记。
    record.entries.clear();
    record.id = id;
  }
  record.entries.push_back(AppliedEntry{yieldType, amount, appliedPopulation});
}

// 注销一个条目并通过 appliedTotal 返回其已应用总量（Amount × 人口基准）。
// 若指针已被复用为其它城市，则丢弃该城市的全部旧条目并返回 false。
bool forgetAppliedCityModifier(void* city, int yieldType, int amount, int& appliedTotal) {
  appliedTotal = 0;
  if (city == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> guard(g_appliedMutex);
  const auto it = g_appliedCities.find(city);
  if (it == g_appliedCities.end()) {
    return false;
  }
  if (!cityRecordMatches(city, it->second)) {
    g_appliedCities.erase(it);
    return false;
  }
  auto& entries = it->second.entries;
  for (auto cursor = entries.begin(); cursor != entries.end(); ++cursor) {
    if (cursor->yieldType == yieldType && cursor->amount == amount) {
      appliedTotal = cursor->amount * cursor->appliedPopulation;
      entries.erase(cursor);
      if (entries.empty()) {
        g_appliedCities.erase(it);
      }
      return true;
    }
  }
  return false;
}

// 人口变化 hook 的补正：把每个已登记条目从各自的人口基准校正到当前人口
// （即追加 Amount × 人口差）。先复制条目再释放锁，避免在调用引擎时持锁
// （引擎会派发事件报告）。
void adjustAppliedCityPopulation(void* city) {
  if (city == nullptr) {
    return;
  }
  const int population = readField<int>(city, kCityPopulationOffset);
  std::vector<AppliedEntry> entries;
  {
    std::lock_guard<std::mutex> guard(g_appliedMutex);
    const auto it = g_appliedCities.find(city);
    if (it == g_appliedCities.end()) {
      return;
    }
    if (!cityRecordMatches(city, it->second)) {
      g_appliedCities.erase(it);
      return;
    }
    entries = it->second.entries;
  }
  const GameCoreApi& api = gameCore();
  if (api.changeYieldModifier == nullptr) {
    return;
  }
  const auto change = reinterpret_cast<ChangeYieldModifierFn>(api.changeYieldModifier);
  for (const AppliedEntry& entry : entries) {
    const int delta = population - entry.appliedPopulation;
    if (delta != 0) {
      change(city, entry.yieldType, entry.amount * delta);
    }
  }
  std::lock_guard<std::mutex> guard(g_appliedMutex);
  const auto it = g_appliedCities.find(city);
  if (it == g_appliedCities.end() || !cityRecordMatches(city, it->second)) {
    return;
  }
  for (AppliedEntry& entry : it->second.entries) {
    entry.appliedPopulation = population;
  }
}

void clearAppliedCityModifiers() {
  std::lock_guard<std::mutex> guard(g_appliedMutex);
  g_appliedCities.clear();
}

// 新建游戏上下文时重新确保 hook 已安装：注册只在进程首次加载插件时执行，
// 而 DllDestroyGameContext 会停用 hook，因此需在此按已注册行为重新启用。
bool installPopulationHookIfNeeded() {
  bool needed = false;
  {
    std::lock_guard<std::mutex> guard(g_behaviorMutex);
    for (const auto& pair : g_behaviors) {
      if (pair.second.behavior == bridge::EffectBehavior::kCityYieldModifierPerPopulation) {
        needed = true;
        break;
      }
    }
  }
  if (!needed) {
    return true;
  }
  return installPopulationHook();
}

// Apply 槽替换：amount 先乘城市人口，再作为百分比写入城市。
extern "C" int ykkz000_perPopulationApply(void* self, void* city) {
  return applyPerPopulation(self, city, 1);
}

// Remove 槽替换：与 Apply 对称取负。
extern "C" int ykkz000_perPopulationRemove(void* self, void* city) {
  return applyPerPopulation(self, city, -1);
}

// 克隆效果对象 vtable 并按函数指针替换 Apply/Remove；未匹配则保留原 vtable，
// 使效果退化为模板行为，避免破坏对象析构。
void* patchEffectObjectVTable(void* effectObject, std::uint32_t /*typeHash*/) {
  const GameCoreApi& api = gameCore();
  if (effectObject == nullptr || api.effectApply == nullptr || api.effectRemove == nullptr) {
    return nullptr;
  }
  auto* source = *reinterpret_cast<void***>(effectObject);
  if (source == nullptr) {
    return nullptr;
  }
  auto* clone = static_cast<void**>(
      HeapAlloc(GetProcessHeap(), 0, sizeof(void*) * kEffectVTableCloneSlots));
  if (clone == nullptr) {
    return nullptr;
  }
  std::memcpy(clone, source, sizeof(void*) * kEffectVTableCloneSlots);

  bool applyPatched = false;
  bool removePatched = false;
  for (std::size_t i = 0; i < kEffectVTableCloneSlots; ++i) {
    if (source[i] == api.effectApply) {
      clone[i] = reinterpret_cast<void*>(&ykkz000_perPopulationApply);
      applyPatched = true;
    } else if (source[i] == api.effectRemove) {
      clone[i] = reinterpret_cast<void*>(&ykkz000_perPopulationRemove);
      removePatched = true;
    }
  }
  if (!applyPatched || !removePatched) {
    logMessage(0, "Custom effect: Apply/Remove slot not matched in effect object vtable; "
                   "keeping template behavior");
    HeapFree(GetProcessHeap(), 0, clone);
    return nullptr;
  }
  *reinterpret_cast<void***>(effectObject) = clone;
  return clone;
}

// 工厂 Create 槽替换：先调用模板 Create（参数解析与引擎一致），再替换对象 vtable。
extern "C" void* ykkz000_customFactoryCreate(void* self, void* outSharedPtr,
                                             const void* params) {
  if (self == nullptr || outSharedPtr == nullptr) {
    return outSharedPtr;
  }
  const auto typeHash = readField<std::uint32_t>(self, kFactoryHashOffset);
  BehaviorRecord record;
  if (!findBehavior(typeHash, record) || record.originalCreate == nullptr) {
    logMessage(0, "Custom effect: missing factory behavior record");
    return outSharedPtr;
  }
  const auto original = reinterpret_cast<FactoryCreateFn>(record.originalCreate);
  original(self, outSharedPtr, params);

  auto* object = *reinterpret_cast<void**>(outSharedPtr);
  if (object != nullptr &&
      record.behavior == bridge::EffectBehavior::kCityYieldModifierPerPopulation) {
    patchEffectObjectVTable(object, typeHash);
  }
  return outSharedPtr;
}

void* customFactoryCreateEntry() {
  return reinterpret_cast<void*>(&ykkz000_customFactoryCreate);
}

} // namespace ykkz000::loader
