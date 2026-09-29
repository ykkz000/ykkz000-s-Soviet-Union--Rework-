#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "loader_internal.h"

// 效果“机制”层：泛化的自定义 EffectType 实现登记 + 工厂 Create 包装 + 效果对象
// vtable 槽替换与归属登记。
//
// 本文件不认识任何具体行为：行为由插件通过 bridge::EffectImpl 提供（Apply/Remove/
// analyze/handlerApply 函数指针），Loader 只按函数指针值在克隆 vtable 中定位并替换
// 对应槽。绝不修改引擎共享的静态 vtable。
namespace ykkz000::loader {
namespace {

// MSVC x64 隐藏返回：shared_ptr 返回经由 RDX 传入，函数需在 RAX 回传同一指针。
using FactoryCreateFn = void* (*)(void* self, void* outSharedPtr, const void* params);

std::mutex g_recordMutex;
std::unordered_map<std::uint32_t, EffectRecord> g_records;

// 由 Loader 克隆并安装到效果对象上的 vtable 块。卸载插件时必须把插件替换过的槽
// 还原为模板原函数，否则引擎后续调用会跳进已卸载内存（最高风险点）。
struct CloneRecord {
  void** block = nullptr;                                    // 块首（含 RTTI 前缀槽）
  void** clone = nullptr;                                    // 交回引擎的 vptr
  void* pluginHandle = nullptr;
  std::vector<std::pair<std::size_t, const void*>> replaced; // 槽号 -> 模板函数
};

std::mutex g_cloneMutex;
std::vector<CloneRecord> g_clones;

} // namespace

bool findEffectRecord(std::uint32_t typeHash, EffectRecord& out) {
  std::lock_guard<std::mutex> guard(g_recordMutex);
  const auto it = g_records.find(typeHash);
  if (it == g_records.end()) {
    return false;
  }
  out = it->second;
  return true;
}

int registerEffectImpl(std::uint32_t typeHash, const bridge::EffectImpl* impl,
                       void* originalCreate) {
  if (typeHash == 0 || impl == nullptr || originalCreate == nullptr) {
    return -1;
  }
  EffectRecord record;
  record.impl = *impl; // 拷贝：插件卸载后仍可安全读取
  record.originalCreate = originalCreate;
  record.pluginHandle = activePluginHandle();
  std::lock_guard<std::mutex> guard(g_recordMutex);
  g_records.insert_or_assign(typeHash, record);
  logMessageF(1, "custom: impl registered hash=0x%08X label=%s apply=%p remove=%p", typeHash,
              record.impl.label != nullptr ? record.impl.label : "(none)",
              reinterpret_cast<void*>(record.impl.apply),
              reinterpret_cast<void*>(record.impl.remove));
  return 0;
}

int unregisterEffectImpl(std::uint32_t typeHash) {
  if (typeHash == 0) {
    return -1;
  }
  std::lock_guard<std::mutex> guard(g_recordMutex);
  return g_records.erase(typeHash) > 0 ? 0 : -1;
}

void* patchEffectObjectSlots(void* effectObject, std::uint32_t typeHash) {
  if (effectObject == nullptr) {
    return nullptr;
  }
  EffectRecord record;
  if (!findEffectRecord(typeHash, record)) {
    logMessageF(0, "Custom effect: missing implementation record hash=0x%08X", typeHash);
    return nullptr;
  }
  const bridge::EffectImpl& impl = record.impl;
  const bool wantApply = impl.apply != nullptr && impl.templateApply != nullptr;
  const bool wantRemove = impl.remove != nullptr && impl.templateRemove != nullptr;
  if (!wantApply && !wantRemove) {
    return nullptr; // 完全复用模板行为
  }

  auto* source = *reinterpret_cast<void***>(effectObject);
  if (source == nullptr) {
    return nullptr;
  }
  const std::size_t slots = kEffectVTableCloneSlots;
  auto* block = static_cast<void**>(
      HeapAlloc(GetProcessHeap(), 0, sizeof(void*) * (slots + civ6::kVTableRttiPrefixSlots)));
  if (block == nullptr) {
    return nullptr;
  }
  block[0] = source[-1]; // MSVC 的 RTTI/COL 指针，必须一并保留
  std::memcpy(block + civ6::kVTableRttiPrefixSlots, source, sizeof(void*) * slots);
  void** clone = block + civ6::kVTableRttiPrefixSlots;

  std::vector<std::pair<std::size_t, const void*>> replaced;
  bool applyPatched = !wantApply;
  bool removePatched = !wantRemove;
  std::size_t applySlot = 0;
  std::size_t removeSlot = 0;
  for (std::size_t i = 0; i < slots; ++i) {
    if (!applyPatched && source[i] == impl.templateApply) {
      clone[i] = reinterpret_cast<void*>(impl.apply);
      applyPatched = true;
      applySlot = i;
      replaced.emplace_back(i, impl.templateApply);
    } else if (!removePatched && source[i] == impl.templateRemove) {
      clone[i] = reinterpret_cast<void*>(impl.remove);
      removePatched = true;
      removeSlot = i;
      replaced.emplace_back(i, impl.templateRemove);
    }
  }
  if (!applyPatched || !removePatched) {
    logMessageF(0, "Custom effect: Apply/Remove slot not matched in effect object vtable; "
                   "keeping template behavior");
    HeapFree(GetProcessHeap(), 0, block);
    return nullptr;
  }

  {
    std::lock_guard<std::mutex> guard(g_cloneMutex);
    CloneRecord cloneRecord;
    cloneRecord.block = block;
    cloneRecord.clone = clone;
    cloneRecord.pluginHandle = record.pluginHandle;
    cloneRecord.replaced = std::move(replaced);
    g_clones.push_back(std::move(cloneRecord));
  }
  logMessageF(1, "custom: patched effect vtable hash=0x%08X label=%s block=%p vtable=%p "
                 "applySlot=%zX removeSlot=%zX",
              typeHash, impl.label != nullptr ? impl.label : "(none)", block, clone, applySlot,
              removeSlot);
  *reinterpret_cast<void***>(effectObject) = clone;
  return block;
}

// 工厂 Create 槽替换：先调用模板 Create（参数解析与引擎一致），再按登记的实现
// 替换对象 vtable 槽。完全泛化，不认识具体行为。
extern "C" void* ykkz000_customFactoryCreate(void* self, void* outSharedPtr,
                                             const void* params) {
  if (self == nullptr || outSharedPtr == nullptr) {
    return outSharedPtr;
  }
  const auto typeHash =
      TryReadOr(self, &civ6::ModifierEffectFactory::type_hash, std::uint32_t{0});
  EffectRecord record;
  if (!findEffectRecord(typeHash, record) || record.originalCreate == nullptr) {
    logMessage(0, "Custom effect: missing factory implementation record");
    return outSharedPtr;
  }
  reinterpret_cast<FactoryCreateFn>(record.originalCreate)(self, outSharedPtr, params);
  void* object = *reinterpret_cast<void**>(outSharedPtr);
  if (object != nullptr) {
    (void)patchEffectObjectSlots(object, typeHash);
  }
  return outSharedPtr;
}

void* customFactoryCreateEntry() {
  return reinterpret_cast<void*>(&ykkz000_customFactoryCreate);
}

void teardownPluginEffects(void* pluginHandle) {
  if (pluginHandle == nullptr) {
    return;
  }
  // 1) 还原该插件替换过的效果对象 vtable 槽（模板原函数仍指向引擎，安全）。
  {
    std::lock_guard<std::mutex> guard(g_cloneMutex);
    for (CloneRecord& record : g_clones) {
      if (record.pluginHandle != pluginHandle) {
        continue;
      }
      for (const auto& entry : record.replaced) {
        record.clone[entry.first] = const_cast<void*>(entry.second);
      }
      record.replaced.clear();
      record.pluginHandle = nullptr;
    }
  }
  // 2) 清空该插件登记的实现回调指针（label/userData 亦为插件内存，一并清空）。
  {
    std::lock_guard<std::mutex> guard(g_recordMutex);
    for (auto& pair : g_records) {
      EffectRecord& record = pair.second;
      if (record.pluginHandle != pluginHandle) {
        continue;
      }
      record.impl.apply = nullptr;
      record.impl.remove = nullptr;
      record.impl.analyze = nullptr;
      record.impl.handlerApply = nullptr;
      record.impl.label = nullptr;
      record.impl.userData = nullptr;
      record.pluginHandle = nullptr;
    }
  }
  clearHandlerCallbacksForPlugin(pluginHandle);
}

} // namespace ykkz000::loader
