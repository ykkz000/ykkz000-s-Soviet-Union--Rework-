#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "loader_internal.h"

namespace ykkz000::loader {
namespace {

// 注册与查询可能来自不同线程（插件注册 vs 引擎 DatabaseWriter 读取），
// 因此对名字表加锁。#unordered_map 为节点式存储，元素指针在 rehash 后仍
// 有效；rememberTypeName 对已存在的 hash 不覆盖值，使已返回的 c_str() 指针
// 在进程生命周期内持续有效（引擎可能长期持有 GetTypeName 的返回值）。
std::mutex g_typeNamesMutex;
std::unordered_map<std::uint32_t, std::string> g_typeNames;

} // namespace

// 替换工厂 vtable 的 GetTypeName 槽：签名等价于
//   const char* GetTypeName() const  (this 位于 RCX，返回值位于 RAX)
extern "C" const char* ykkz000_GetTypeName(void* self) {
  if (self == nullptr) {
    return "";
  }
  const auto hash = TryReadOr(self, &civ6::ModifierEffectFactory::type_hash, std::uint32_t{0});
  static std::atomic<bool> s_logged{false};
  if (!s_logged.exchange(true)) {
    logMessageF(1, "getname: first call self=%p hash=0x%08X", self, hash);
  }

  std::lock_guard<std::mutex> guard(g_typeNamesMutex);
  const auto it = g_typeNames.find(hash);
  return it != g_typeNames.end() ? it->second.c_str() : "";
}

void rememberTypeName(std::uint32_t typeHash, const char* typeName) {
  if (typeName == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> guard(g_typeNamesMutex);
  g_typeNames.emplace(typeHash, std::string(typeName));
}

void forgetTypeName(std::uint32_t typeHash) {
  std::lock_guard<std::mutex> guard(g_typeNamesMutex);
  g_typeNames.erase(typeHash);
}

void* cloneFactoryVTable(void* templateFactory) {
  if (templateFactory == nullptr) {
    return nullptr;
  }
  auto* source = *reinterpret_cast<void***>(templateFactory);
  if (source == nullptr) {
    return nullptr;
  }
  logMessageF(1, "clone: template=%p source vtable=%p slots=%zu", templateFactory, source,
              civ6::kFactoryVTableSlots);
  const std::size_t slots = civ6::kFactoryVTableSlots;
  auto* block = static_cast<void**>(
      HeapAlloc(GetProcessHeap(), 0, sizeof(void*) * (slots + civ6::kVTableRttiPrefixSlots)));
  if (block == nullptr) {
    return nullptr;
  }
  block[0] = source[-1]; // MSVC 的 RTTI/COL 指针，必须一并保留
  std::memcpy(block + civ6::kVTableRttiPrefixSlots, source, sizeof(void*) * slots);

  void** vtable = block + civ6::kVTableRttiPrefixSlots; // 交回引擎与调用方的 vptr
  vtable[civ6::kFactoryTypeNameSlot] = reinterpret_cast<void*>(&ykkz000_GetTypeName);
  logMessageF(1, "clone: block=%p vtable=%p nameSlot=%zX rtti=%p", block, vtable,
              civ6::kFactoryTypeNameSlot, block[0]);
  return vtable;
}

} // namespace ykkz000::loader
