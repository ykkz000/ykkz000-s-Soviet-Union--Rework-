#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_set>

#include "loader_internal.h"

namespace ykkz000::loader {
namespace {

std::mutex g_registryMutex;
std::unordered_set<std::uint32_t> g_registeredHashes;

using MallocTempFn = void* (*)(std::size_t size, const char* file, int line, int a, int b);
using ReserveVectorFn = void (*)(void* vector, std::size_t count);
using GetTypeIdFn = std::uint32_t (*)(void* factory);

void* readPointer(void* base, std::size_t offset) {
  return *reinterpret_cast<void**>(static_cast<std::uint8_t*>(base) + offset);
}

void writePointer(void* base, std::size_t offset, void* value) {
  *reinterpret_cast<void**>(static_cast<std::uint8_t*>(base) + offset) = value;
}

std::uint32_t typeIdOf(void* factory) {
  if (factory == nullptr) {
    return 0;
  }
  auto* vtable = *reinterpret_cast<void***>(factory);
  if (vtable == nullptr) {
    return 0;
  }
  const auto getTypeId = reinterpret_cast<GetTypeIdFn>(vtable[kFactoryTypeIdSlot]);
  return getTypeId != nullptr ? getTypeId(factory) : 0;
}

void* findTemplateFactory(void* vector, std::uint32_t templateHash) {
  auto* begin = static_cast<void**>(readPointer(vector, kVectorBeginOffset));
  auto* end = static_cast<void**>(readPointer(vector, kVectorEndOffset));
  for (auto* cursor = begin; cursor != end; ++cursor) {
    if (typeIdOf(*cursor) == templateHash) {
      return *cursor;
    }
  }
  return nullptr;
}

} // namespace

int registerEffectType(const bridge::EffectDesc* desc) {
  LogScope scope("register effect type");
  if (desc == nullptr || desc->typeName == nullptr || desc->templateEffect == nullptr) {
    logMessage(0, "registerEffectType: invalid argument");
    return -1;
  }

  if (!ensureGameCoreLoaded()) {
    logMessage(0, "registerEffectType: real GameCore unavailable");
    return -2;
  }
  const GameCoreApi& api = gameCore();
  if (api.getEffectRegistry == nullptr || api.mallocTemp == nullptr) {
    logMessage(0, "registerEffectType: GameCore entry points missing");
    return -2;
  }

  const std::uint32_t typeHash = makeHash(desc->typeName);
  const std::uint32_t templateHash = makeHash(desc->templateEffect);

  std::lock_guard<std::mutex> guard(g_registryMutex);
  if (g_registeredHashes.find(typeHash) != g_registeredHashes.end()) {
    return 0; // 已注册，幂等
  }

  const auto getRegistry = reinterpret_cast<bridge::GetEffectRegistryFn>(api.getEffectRegistry);
  void* vector = getRegistry();
  void* templateFactory = findTemplateFactory(vector, templateHash);
  if (templateFactory == nullptr) {
    logMessage(0, "registerEffectType: template effect factory not found");
    return -3;
  }

  void* vtable = cloneFactoryVTable(templateFactory);
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
    if (templateCreate == nullptr) {
      logMessage(0, "registerEffectType: template factory is missing the Create slot");
      return -8;
    }
    static_cast<void**>(vtable)[kFactoryCreateSlot] = customFactoryCreateEntry();
    registerEffectBehavior(typeHash, desc->behavior, templateCreate);
    if (desc->behavior == bridge::EffectBehavior::kCityYieldModifierPerPopulation &&
        !installPopulationHook()) {
      logMessage(1, "Per-population effect: population hook not installed; modifier will "
                     "use the founding-time population snapshot");
    }
  }

  rememberTypeName(typeHash, desc->typeName);

  auto* factory = reinterpret_cast<MallocTempFn>(api.mallocTemp)(
      kFactoryObjectSize, "ykkz000_loader", 0, 0, 0);
  if (factory == nullptr) {
    logMessage(0, "registerEffectType: failed to allocate factory object");
    return -5;
  }
  *reinterpret_cast<void**>(factory) = vtable;
  *reinterpret_cast<std::uint32_t*>(
      static_cast<std::uint8_t*>(factory) + kFactoryHashOffset) = typeHash;

  if (readPointer(vector, kVectorEndOffset) == readPointer(vector, kVectorCapacityOffset)) {
    if (api.reserveVector == nullptr) {
      logMessage(0, "registerEffectType: registry full and no reserve entry");
      return -6;
    }
    reinterpret_cast<ReserveVectorFn>(api.reserveVector)(vector, 1);
  }

  auto* end = static_cast<std::uint8_t*>(readPointer(vector, kVectorEndOffset));
  if (end == nullptr) {
    logMessage(0, "registerEffectType: registry cursor is null");
    return -7;
  }
  *reinterpret_cast<void**>(end) = factory;
  writePointer(vector, kVectorEndOffset, end + sizeof(void*));

  g_registeredHashes.insert(typeHash);
  return 0;
}

} // namespace ykkz000::loader
