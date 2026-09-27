#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "loader_internal.h"

namespace ykkz000::loader {
namespace {

std::mutex g_registryMutex;
std::unordered_set<std::uint32_t> g_registeredHashes;
std::vector<RegisteredEffect> g_registeredEffects;

using MallocTempFn = void* (*)(std::size_t size, const char* file, int line, int a, int b);
using ReserveVectorFn = void (*)(void* vector, std::size_t count);

void* readPointer(void* base, std::size_t offset) {
  return *reinterpret_cast<void**>(static_cast<std::uint8_t*>(base) + offset);
}

void writePointer(void* base, std::size_t offset, void* value) {
  *reinterpret_cast<void**>(static_cast<std::uint8_t*>(base) + offset) = value;
}

// 直接读取工厂对象 +0x08 的类型哈希，绝不调用引擎虚函数：`GetTypeId()` 等价于
// 返回该字段，而按猜测签名调用 vtable 触发过 /GS 栈 cookie 快速失败。
std::uint32_t typeIdOf(void* factory) {
  if (factory == nullptr) {
    return 0;
  }
  return *reinterpret_cast<const std::uint32_t*>(
      static_cast<const std::uint8_t*>(factory) + kFactoryHashOffset);
}

void* findTemplateFactory(void* vector, std::uint32_t templateHash) {
  auto* begin = static_cast<void**>(readPointer(vector, kVectorBeginOffset));
  auto* end = static_cast<void**>(readPointer(vector, kVectorEndOffset));
  const std::ptrdiff_t count =
      (begin != nullptr && end != nullptr) ? (end - begin) : -1;
  logMessageF(1, "reg: registry begin=%p end=%p count=%lld", begin, end,
              static_cast<long long>(count));
  if (begin == nullptr || end == nullptr || end < begin || count > 4096) {
    logMessage(0, "reg: registry iterators look invalid; aborting iteration");
    return nullptr;
  }
  for (auto* cursor = begin; cursor != end; ++cursor) {
    if (typeIdOf(*cursor) == templateHash) {
      logMessageF(1, "reg: template matched factory=%p hash=0x%08X", *cursor, templateHash);
      return *cursor;
    }
  }
  logMessageF(1, "reg: template hash 0x%08X not found in %lld entries", templateHash,
              static_cast<long long>(count));
  return nullptr;
}

} // namespace

std::vector<RegisteredEffect> registeredEffects() {
  std::lock_guard<std::mutex> guard(g_registryMutex);
  return g_registeredEffects;
}

int registerEffectType(const bridge::EffectDesc* desc) {
  LogScope scope("register effect type");
  if (desc == nullptr || desc->typeName == nullptr || desc->templateEffect == nullptr) {
    logMessage(0, "registerEffectType: invalid argument");
    return -1;
  }

  logMessageF(1, "reg: enter type=%s template=%s behavior=%d", desc->typeName,
              desc->templateEffect, static_cast<int>(desc->behavior));

  if (!ensureGameCoreLoaded()) {
    logMessage(0, "registerEffectType: real GameCore unavailable");
    return -2;
  }
  logMessageF(1, "reg: GameCore ready");
  const GameCoreApi& api = gameCore();
  if (api.getEffectRegistry == nullptr || api.mallocTemp == nullptr) {
    logMessage(0, "registerEffectType: GameCore entry points missing");
    return -2;
  }
  logMessageF(1, "reg: api module=%p getRegistry=%p mallocTemp=%p reserve=%p", api.module,
              api.getEffectRegistry, api.mallocTemp, api.reserveVector);
  logMessageF(1,
              "reg: api effectApply=%p effectRemove=%p changeYieldModifier=%p "
              "changePopulation=%p",
              api.effectApply, api.effectRemove, api.changeYieldModifier, api.changePopulation);

  const std::uint32_t typeHash = makeHash(desc->typeName);
  const std::uint32_t templateHash = makeHash(desc->templateEffect);
  logMessageF(1, "reg: typeHash=0x%08X templateHash=0x%08X", typeHash, templateHash);

  std::lock_guard<std::mutex> guard(g_registryMutex);
  logMessageF(1, "reg: registry lock acquired");
  if (g_registeredHashes.find(typeHash) != g_registeredHashes.end()) {
    return 0; // 已注册，幂等
  }

  const auto getRegistry = reinterpret_cast<bridge::GetEffectRegistryFn>(api.getEffectRegistry);
  logMessageF(1, "reg: calling GetTypes @%p", api.getEffectRegistry); // 崩溃前最后一条
  void* vector = getRegistry();
  logMessageF(1, "reg: GetTypes returned vector=%p", vector);         // 崩溃后下一条
  void* templateFactory = findTemplateFactory(vector, templateHash);
  logMessageF(1, "reg: templateFactory=%p", templateFactory);
  if (templateFactory == nullptr) {
    logMessage(0, "registerEffectType: template effect factory not found");
    return -3;
  }

  void* vtable = cloneFactoryVTable(templateFactory);
  logMessageF(1, "reg: factory vtable clone=%p", vtable);
  if (vtable == nullptr) {
    logMessage(0, "registerEffectType: failed to clone factory vtable");
    return -4;
  }

  // 自定义行为：把克隆工厂的 Create 槽指向本 Loader 的入口，并记录模板 Create
  // 以便在入口内部复用引擎的参数解析。
  if (desc->behavior != bridge::EffectBehavior::kInherit) {
    if (desc->behavior != bridge::EffectBehavior::kCityYieldModifierPerPopulation) {
      logMessage(0, "registerEffectType: unknown custom effect behavior");
      return -8;
    }
    auto* templateVtable = *reinterpret_cast<void***>(templateFactory);
    void* templateCreate =
        templateVtable != nullptr ? templateVtable[kFactoryCreateSlot] : nullptr;
    logMessageF(1, "reg: behavior=%d templateCreate=%p", static_cast<int>(desc->behavior),
                templateCreate);
    if (templateCreate == nullptr) {
      logMessage(0, "registerEffectType: template factory is missing the Create slot");
      return -8;
    }
    static_cast<void**>(vtable)[kFactoryCreateSlot] = customFactoryCreateEntry();
    registerEffectBehavior(typeHash, desc->behavior, templateCreate);
    logMessageF(1, "reg: behavior recorded");
    if (desc->behavior == bridge::EffectBehavior::kCityYieldModifierPerPopulation) {
      const bool hookReady = installPopulationHook();
      logMessageF(1, "reg: installPopulationHook -> %d", hookReady ? 1 : 0);
      if (!hookReady) {
        logMessage(1, "Per-population effect: population hook not installed; modifier will "
                       "use the founding-time population snapshot");
      }
    }
  }

  rememberTypeName(typeHash, desc->typeName);

  auto* factory = reinterpret_cast<MallocTempFn>(api.mallocTemp)(
      kFactoryObjectSize, "ykkz000_loader", 0, 0, 0);
  logMessageF(1, "reg: factory allocated=%p size=0x%zX", factory, kFactoryObjectSize);
  if (factory == nullptr) {
    logMessage(0, "registerEffectType: failed to allocate factory object");
    return -5;
  }
  *reinterpret_cast<void**>(factory) = vtable;
  *reinterpret_cast<std::uint32_t*>(
      static_cast<std::uint8_t*>(factory) + kFactoryHashOffset) = typeHash;
  logMessageF(1, "reg: factory fields written");

  logMessageF(1, "reg: vector end=%p cap=%p", readPointer(vector, kVectorEndOffset),
              readPointer(vector, kVectorCapacityOffset));
  if (readPointer(vector, kVectorEndOffset) == readPointer(vector, kVectorCapacityOffset)) {
    if (api.reserveVector == nullptr) {
      logMessage(0, "registerEffectType: registry full and no reserve entry");
      return -6;
    }
    reinterpret_cast<ReserveVectorFn>(api.reserveVector)(vector, 1);
    logMessageF(1, "reg: reserve called");
  }
  logMessageF(1, "reg: vector end after=%p", readPointer(vector, kVectorEndOffset));

  auto* end = static_cast<std::uint8_t*>(readPointer(vector, kVectorEndOffset));
  if (end == nullptr) {
    logMessage(0, "registerEffectType: registry cursor is null");
    return -7;
  }
  *reinterpret_cast<void**>(end) = factory;
  writePointer(vector, kVectorEndOffset, end + sizeof(void*));

  g_registeredHashes.insert(typeHash);
  g_registeredEffects.push_back(
      RegisteredEffect{typeHash, std::string(desc->typeName), desc->behavior});
  logMessageF(1, "reg: done hash=0x%08X", typeHash);
  return 0;
}

} // namespace ykkz000::loader
