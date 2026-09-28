#pragma once

#include <cstdint>

// 宿主（Loader）与插件之间的纯 C ABI 边界。
// 仅使用 POD、函数指针与 const char*；不跨 DLL 传递 std:: 对象或异常。
namespace ykkz000::bridge {

inline constexpr std::uint32_t kHostApiVersion = 3;

// 自定义效果行为：决定 Loader 是否为该 EffectType 安装专属处理器。
enum class EffectBehavior : std::int32_t {
  kInherit = 0,                        // 完全复用模板行为
  kCityYieldModifierPerPopulation = 1, // 每人口 × Amount% 的城市产出修正
};

// 注意：GameEffects 元数据由引擎依模板的 GetTypeInfo 写入，
// 自定义元数据在当前机制下不受支持，故此处不提供相应字段。
struct EffectDesc {
  const char* typeName;          // 必填
  const char* templateEffect;    // 必填，复用其行为与参数定义的已有效果名
  EffectBehavior behavior;       // kInherit = 复用模板行为
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
