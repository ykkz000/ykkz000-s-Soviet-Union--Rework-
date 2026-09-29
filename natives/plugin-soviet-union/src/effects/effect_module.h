#pragma once

#include <ykkz000/bridge.h>

// 插件侧效果模块接口：把“一个自定义效果”的全部装配与生命周期收敛到一个静态实例。
// 效果实现放各自的 .cpp 内（自行装配常驻的 EffectImpl/EffectDesc），plugin.cpp 只做
// 清单遍历，不认识任何具体效果的名字、字段与开关。
namespace ykkz000::plugin {

struct EffectModule {
  const char* name;                                      // 诊断名
  const bridge::EffectDesc* (*describe)(const bridge::Host& host); // 装配并由调用方注册
  void (*on_context)(bridge::GameContextEvent event, void* context); // 可空
  void (*shutdown)();                                    // 可空
};

} // namespace ykkz000::plugin
