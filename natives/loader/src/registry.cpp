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
  return TryReadOr(factory, &civ6::ModifierEffectFactory::type_hash, std::uint32_t{0});
}

void* findTemplateFactory(void* vector, std::uint32_t templateHash) {
  auto* begin = static_cast<void**>(readPointer(vector, offsetof(civ6::VectorView<void*>, begin)));
  auto* end = static_cast<void**>(readPointer(vector, offsetof(civ6::VectorView<void*>, end)));
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

// 校验插件提供的实现自洽：声明替换 Apply/Remove 槽时必须给出对应的模板函数指针，
// 否则 Loader 无法在克隆 vtable 中定位槽。
bool isImplConsistent(const bridge::EffectImpl* impl) {
  if (impl == nullptr) {
    return true;
  }
  if (impl->apply != nullptr && impl->templateApply == nullptr) {
    return false;
  }
  if (impl->remove != nullptr && impl->templateRemove == nullptr) {
    return false;
  }
  return true;
}

// 从引擎注册表向量移除“刚追加”的工厂对象：把 end 指针回退一格并清空该槽。仅在末项
// 确实指向本次追加的 factory 时才动，避免异常状态下误删他人条目。
void removeLastFactory(void* vector, void* factory) {
  if (vector == nullptr || factory == nullptr) {
    return;
  }
  auto* begin = static_cast<std::uint8_t*>(
      readPointer(vector, offsetof(civ6::VectorView<void*>, begin)));
  auto* end = static_cast<std::uint8_t*>(
      readPointer(vector, offsetof(civ6::VectorView<void*>, end)));
  if (begin == nullptr || end == nullptr || end <= begin) {
    return;
  }
  auto* last = reinterpret_cast<void**>(end) - 1;
  if (*last != factory) {
    return;
  }
  *last = nullptr;
  writePointer(vector, offsetof(civ6::VectorView<void*>, end), last);
}

// registerEffectType 的失败回滚，调用方须持有 g_registryMutex：撤销本次登记的全部痕迹
// （工厂向量项、typeHash、已注册效果记录、impl 记录与类型名）。未登记成功的部分为 no-op。
void rollbackRegistrationLocked(std::uint32_t typeHash, void* vector, void* factory) {
  logMessageF(0, "registerEffectType: rolling back hash=0x%08X", typeHash);
  removeLastFactory(vector, factory);
  (void)unregisterEffectImpl(typeHash);
  forgetTypeName(typeHash);
  g_registeredHashes.erase(typeHash);
  for (auto it = g_registeredEffects.begin(); it != g_registeredEffects.end(); ++it) {
    if (it->hash == typeHash) {
      g_registeredEffects.erase(it);
      break;
    }
  }
}

} // namespace

std::vector<RegisteredEffect> registeredEffects() {
  std::lock_guard<std::mutex> guard(g_registryMutex);
  return g_registeredEffects;
}

bool isRegisteredTemplateHash(std::uint32_t hash) {
  if (hash == 0) {
    return false;
  }
  std::lock_guard<std::mutex> guard(g_registryMutex);
  for (const RegisteredEffect& effect : g_registeredEffects) {
    if (effect.templateHash == hash) {
      return true;
    }
  }
  return false;
}

// 自定义效果自身的类型哈希（工厂对象 +0x08），区别于 isRegisteredTemplateHash 的
// 模板哈希。模板 Create 旁路诊断据此把对象标注为 custom / built-in。
bool isRegisteredHash(std::uint32_t typeHash) {
  if (typeHash == 0) {
    return false;
  }
  std::lock_guard<std::mutex> guard(g_registryMutex);
  return g_registeredHashes.find(typeHash) != g_registeredHashes.end();
}

int registerEffectType(const bridge::EffectDesc* desc) {
  LogScope scope("register effect type");
  if (desc == nullptr || desc->typeName == nullptr || desc->templateEffect == nullptr) {
    logMessage(0, "registerEffectType: invalid argument");
    return -1;
  }
  if (!isImplConsistent(desc->impl)) {
    logMessage(0, "registerEffectType: impl declares a slot replacement without template fn");
    return -8;
  }

  const bool custom = desc->impl != nullptr;
  logMessageF(1, "reg: enter type=%s template=%s custom=%d", desc->typeName,
              desc->templateEffect, custom ? 1 : 0);

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

  // 自定义行为：把克隆工厂的 Create 槽指向 Loader 的泛化入口，并登记插件实现
  // （实现细节由插件提供，Loader 不再有任何行为分支）。
  if (custom) {
    auto* templateVtable = *reinterpret_cast<void***>(templateFactory);
    void* templateCreate =
        templateVtable != nullptr ? templateVtable[civ6::kFactoryCreateSlot] : nullptr;
    logMessageF(1, "reg: custom impl templateCreate=%p", templateCreate);
    if (templateCreate == nullptr) {
      logMessage(0, "registerEffectType: template factory is missing the Create slot");
      return -8;
    }
    static_cast<void**>(vtable)[civ6::kFactoryCreateSlot] = customFactoryCreateEntry();
    if (registerEffectImpl(typeHash, desc->impl, templateCreate) != 0) {
      logMessage(0, "registerEffectType: failed to record implementation");
      return -8;
    }
  }

  rememberTypeName(typeHash, desc->typeName);

  auto* factory = reinterpret_cast<MallocTempFn>(api.mallocTemp)(
      sizeof(civ6::ModifierEffectFactory), "ykkz000_loader", 0, 0, 0);
  logMessageF(1, "reg: factory allocated=%p size=0x%zX", factory,
              sizeof(civ6::ModifierEffectFactory));
  if (factory == nullptr) {
    logMessage(0, "registerEffectType: failed to allocate factory object");
    return -5;
  }
  *reinterpret_cast<void**>(factory) = vtable;
  (void)TryWrite(factory, &civ6::ModifierEffectFactory::type_hash, typeHash);
  logMessageF(1, "reg: factory fields written");

  logMessageF(1, "reg: vector end=%p cap=%p",
              readPointer(vector, offsetof(civ6::VectorView<void*>, end)),
              readPointer(vector, offsetof(civ6::VectorView<void*>, capacity)));
  if (readPointer(vector, offsetof(civ6::VectorView<void*>, end)) ==
      readPointer(vector, offsetof(civ6::VectorView<void*>, capacity))) {
    if (api.reserveVector == nullptr) {
      logMessage(0, "registerEffectType: registry full and no reserve entry");
      return -6;
    }
    reinterpret_cast<ReserveVectorFn>(api.reserveVector)(vector, 1);
    logMessageF(1, "reg: reserve called");
  }
  logMessageF(1, "reg: vector end after=%p",
              readPointer(vector, offsetof(civ6::VectorView<void*>, end)));

  auto* end = static_cast<std::uint8_t*>(
      readPointer(vector, offsetof(civ6::VectorView<void*>, end)));
  if (end == nullptr) {
    logMessage(0, "registerEffectType: registry cursor is null");
    return -7;
  }
  *reinterpret_cast<void**>(end) = factory;
  writePointer(vector, offsetof(civ6::VectorView<void*>, end), end + sizeof(void*));

  g_registeredHashes.insert(typeHash);
  g_registeredEffects.push_back(
      RegisteredEffect{typeHash, templateHash, std::string(desc->typeName)});
  logMessageF(1, "reg: done hash=0x%08X", typeHash);

  // 唯一的 hook 安装入口：注册期在工厂/实现已登记后调用插件提供的 prepare。返回非 0
  // 即视为注册失败：先移除本批次新建的 hook（兜底；主责是 prepare 自行撤销半程安装），
  // 再回滚本次登记并返回错误码。
  if (desc->prepare != nullptr) {
    logMessageF(1, "reg: invoking prepare hash=0x%08X", typeHash);
    beginHookScope();
    const int prepare_status = desc->prepare(desc->userData);
    if (prepare_status != 0) {
      endHookScope(true);
      logMessageF(0, "registerEffectType: prepare returned %d", prepare_status);
      rollbackRegistrationLocked(typeHash, vector, factory);
      return -9;
    }
    endHookScope(false);
  }
  return 0;
}

} // namespace ykkz000::loader
