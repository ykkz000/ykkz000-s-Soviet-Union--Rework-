#pragma once

#include <cstdint>

// 宿主（Loader）与插件之间的纯 C ABI 边界。
// 仅使用 POD、函数指针与 const char*；不跨 DLL 传递 std:: 对象或异常。
namespace ykkz000::bridge {

inline constexpr std::uint32_t kHostApiVersion = 1;

struct EffectDesc {
  const char* typeName;          // 必填，如 "EFFECT_YKKZ000_SOVIET_FIVE_YEAR_PLAN"
  const char* templateEffect;    // 必填，复用行为的已有效果名
  const char* commonName;        // 可空 = 继承模板
  const char* description;
  const char* tags;
  const char* gameCapabilities;
  const char* contextInterfaces;
  const char* subjectInterfaces;
  int         supportsRemove;    // -1 = 继承模板
};

using MakeHashFn           = std::uint32_t (*)(const char*);
using RegisterEffectTypeFn = int (*)(const EffectDesc*);
using LogFn                = void (*)(int level, const char* msg);
using GetEffectRegistryFn  = void* (*)();

struct Host {
  std::uint32_t        apiVersion;
  MakeHashFn           makeHash;
  RegisterEffectTypeFn registerEffectType;
  LogFn                log;
  void*                gameCoreModule;
  GetEffectRegistryFn  getEffectRegistry;
};

using GetPluginFn     = int  (*)(Host* host);
using DestroyPluginFn = void (*)();

} // namespace ykkz000::bridge

#define YKKZ000_PLUGIN_EXPORT_GETPLUGIN  "GetPlugin"
#define YKKZ000_PLUGIN_EXPORT_DESTROY    "DestroyPlugin"
