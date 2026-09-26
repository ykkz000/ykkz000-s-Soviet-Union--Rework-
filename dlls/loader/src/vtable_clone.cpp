#include <windows.h>

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
// 有效，故返回的 c_str() 在元素未被覆盖时保持稳定。
std::mutex g_typeNamesMutex;
std::unordered_map<std::uint32_t, std::string> g_typeNames;

} // namespace

// 替换工厂 vtable 的 GetTypeName 槽：签名等价于
//   const char* GetTypeName() const  (this 位于 RCX，返回值位于 RAX)
extern "C" const char* ykkz000_GetTypeName(void* self) {
  if (self == nullptr) {
    return "";
  }
  const auto hash = *reinterpret_cast<const std::uint32_t*>(
      static_cast<const std::uint8_t*>(self) + kFactoryHashOffset);

  std::lock_guard<std::mutex> guard(g_typeNamesMutex);
  const auto it = g_typeNames.find(hash);
  return it != g_typeNames.end() ? it->second.c_str() : "";
}

void rememberTypeName(std::uint32_t typeHash, const char* typeName) {
  if (typeName == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> guard(g_typeNamesMutex);
  g_typeNames.insert_or_assign(typeHash, std::string(typeName));
}

void* cloneFactoryVTable(void* templateFactory) {
  if (templateFactory == nullptr) {
    return nullptr;
  }
  auto* source = *reinterpret_cast<void***>(templateFactory);
  if (source == nullptr) {
    return nullptr;
  }
  const std::size_t bytes = sizeof(void*) * kFactoryVTableSlots;
  auto* clone = static_cast<void**>(HeapAlloc(GetProcessHeap(), 0, bytes));
  if (clone == nullptr) {
    return nullptr;
  }
  std::memcpy(clone, source, bytes);
  clone[kFactoryTypeNameSlot] = reinterpret_cast<void*>(&ykkz000_GetTypeName);
  return clone;
}

} // namespace ykkz000::loader
