#include <ykkz000/bridge.h>
#include <ykkz000/export.h>

#include "effects/effect_modules.h"
#include "effects/engine_access.h"

// 苏联模组的策略层：向 Loader 登记自定义 EffectType，并把上下文事件转发给各效果模块。
// 本文件不认识任何具体效果：装配与开关都在各模块自己的 .cpp 内，这里只做清单遍历。
namespace {

void OnGameContext(ykkz000::bridge::GameContextEvent event, void* context) {
  for (const ykkz000::plugin::EffectModule* module : ykkz000::plugin::AllEffectModules()) {
    if (module->on_context != nullptr) {
      module->on_context(event, context);
    }
  }
}

int RegisterAll(ykkz000::bridge::Host* host) {
  for (const ykkz000::plugin::EffectModule* module : ykkz000::plugin::AllEffectModules()) {
    const ykkz000::bridge::EffectDesc* desc = module->describe(*host);
    if (desc == nullptr) {
      return -1;
    }
    // registerEffectType 内部在登记后调用 desc->prepare；失败时已回滚本次注册。
    const int result = host->registerEffectType(desc);
    if (result != 0) {
      return result;
    }
  }
  return 0;
}

} // namespace

YKKZ000_PLUGIN_API int GetPlugin(ykkz000::bridge::Host* host) {
  if (host == nullptr || host->apiVersion < ykkz000::bridge::kHostApiVersion ||
      host->registerEffectType == nullptr || host->engine == nullptr) {
    return 0;
  }
  ykkz000::plugin::SetContext(host);
  host->onGameContext = &OnGameContext; // 登记上下文回调，Loader 据此广播
  return RegisterAll(host) == 0 ? 1 : 0;
}

YKKZ000_PLUGIN_API void DestroyPlugin() {
  for (const ykkz000::plugin::EffectModule* module : ykkz000::plugin::AllEffectModules()) {
    if (module->shutdown != nullptr) {
      module->shutdown();
    }
  }
  ykkz000::plugin::SetContext(nullptr);
}
