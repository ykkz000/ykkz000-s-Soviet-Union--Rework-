#include <ykkz000/bridge.h>
#include <ykkz000/export.h>

using namespace ykkz000::bridge;

namespace {

int registerEffects(Host* host) {
  EffectDesc desc = {};
  desc.typeName = "EFFECT_YKKZ000_SOVIET_FIVE_YEAR_PLAN";
  desc.templateEffect = "EFFECT_ADJUST_ALL_DISTRICTS_PRODUCTION";
  desc.commonName = nullptr;
  desc.description = nullptr;
  desc.tags = nullptr;
  desc.gameCapabilities = nullptr;
  desc.contextInterfaces = nullptr;
  desc.subjectInterfaces = nullptr;
  desc.supportsRemove = -1;
  return host->registerEffectType(&desc);
}

} // namespace

YKKZ000_PLUGIN_API int GetPlugin(Host* host) {
  if (host == nullptr || host->apiVersion < kHostApiVersion ||
      host->registerEffectType == nullptr) {
    return 0;
  }
  return registerEffects(host) == 0 ? 1 : 0;
}

YKKZ000_PLUGIN_API void DestroyPlugin() {
}
