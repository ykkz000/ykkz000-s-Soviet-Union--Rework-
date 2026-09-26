#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>

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

// 与 Effects::AdjustCityYieldModifier 相同的遍历结构，但把 Amount 乘以城市人口，
// sign 用于 Apply(+1)/Remove(-1)。
int applyPerPopulation(void* self, void* city, int sign) {
  const GameCoreApi& api = gameCore();
  if (self == nullptr || city == nullptr || api.changeYieldModifier == nullptr) {
    return 0;
  }
  const int population = readField<int>(city, kCityPopulationOffset);
  const int entryCount = readField<int>(self, kEffectEntryCountOffset);
  const int amountCount = readField<int>(self, kEffectAmountCountOffset);
  const int* const yields = readField<const int*>(self, kEffectYieldTypeArrayOffset);
  const int* const amounts = readField<const int*>(self, kEffectAmountArrayOffset);
  const auto change = reinterpret_cast<ChangeYieldModifierFn>(api.changeYieldModifier);
  for (int i = 0; i < entryCount && i < amountCount; ++i) {
    change(city, yields[i], amounts[i] * population * sign);
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
    logMessage(0, "自定义效果：效果对象 vtable 未匹配到 Apply/Remove，保留模板行为");
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
    logMessage(0, "自定义效果：缺少工厂行为记录");
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
