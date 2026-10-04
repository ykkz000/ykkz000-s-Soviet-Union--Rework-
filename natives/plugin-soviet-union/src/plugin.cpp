#include <ykkz000/bridge/host.h>
#include <ykkz000/export.h>

#include "effects/effect_modules.h"
#include "effects/engine_access.h"
#include "effects/extra_persistence.h"

// Strategy layer of the Soviet Union mod: registers the custom EffectTypes with the Loader and
// forwards context events to each effect module.
// This file knows no concrete effect: assembly and toggles live in each module's own .cpp; this
// file only iterates the manifest.
namespace {

void OnGameContext(ykkz000::bridge::GameContextEvent event, void* context) {
  if (event == ykkz000::bridge::GameContextEvent::kCreated) {
    // Install the AutoVariable persistence hooks before any City/Unit is constructed in the new
    // context; the effect modules then mirror/hydrate their side tables through them.
    (void)ykkz000::plugin::EnsurePersistenceHooks();
  }
  for (const ykkz000::plugin::EffectModule* module : ykkz000::plugin::AllEffectModules()) {
    if (module->on_context != nullptr) {
      module->on_context(event, context);
    }
  }
  if (event == ykkz000::bridge::GameContextEvent::kDestroyed) {
    ykkz000::plugin::ShutdownPersistence();
  }
}

int RegisterAll(ykkz000::bridge::Host* host) {
  for (const ykkz000::plugin::EffectModule* module : ykkz000::plugin::AllEffectModules()) {
    const ykkz000::bridge::EffectDesc* desc = module->describe(*host);
    if (desc == nullptr) {
      return -1;
    }
    // registerEffectType internally calls desc->prepare after registering; on failure it has
    // already rolled back this registration.
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
  host->onGameContext = &OnGameContext; // Register the context callback the Loader broadcasts through.
  return RegisterAll(host) == 0 ? 1 : 0;
}

YKKZ000_PLUGIN_API void DestroyPlugin() {
  for (const ykkz000::plugin::EffectModule* module : ykkz000::plugin::AllEffectModules()) {
    if (module->shutdown != nullptr) {
      module->shutdown();
    }
  }
  ykkz000::plugin::ResetPersistenceForUnload();
  ykkz000::plugin::SetContext(nullptr);
}
