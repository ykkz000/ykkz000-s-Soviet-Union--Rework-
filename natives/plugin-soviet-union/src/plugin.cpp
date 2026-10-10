#include <ykkz000/bridge/host.h>
#include <ykkz000/export.h>

#include <ykkz000/plugin/effects.h>
#include <ykkz000/plugin/city_yield_common.h>
#include <ykkz000/plugin/effect.h>
#include <ykkz000/plugin/effecttype_api.h>
#include <ykkz000/plugin/object_lifecycle.h>
#include <ykkz000/plugin/persistence_api.h>

#include <ykkz000/civ6/common.h>

// Consumer entry point of the Soviet Union mod.
//
// This plugin depends on two API plugins (declared in GetPluginManifest): the loader's dependency
// topological order initializes them first, so by the time GetPlugin runs their exports are ready.
// The effecttype API provides the shared Effect interface, engine access, and the registration
// driver; the persistence API provides variable-description-driven AutoVariable persistence and
// City/Unit lifecycle notifications. This file only assembles those services and iterates the effect
// manifest -- it knows no concrete effect.
namespace {

const char* const kDependencies[] = {"ykkz000.api.effecttype", "ykkz000.api.persistence"};

constexpr ykkz000::bridge::PluginManifest kManifest = {
    sizeof(ykkz000::bridge::PluginManifest), // structSize
    ykkz000::bridge::kHostApiVersion,        // apiVersion
    "ykkz000.soviet_union",                  // name
    1,                                       // versionMajor
    0,                                       // versionMinor
    0,                                       // versionPatch
    kDependencies,                           // dependencies
    sizeof(kDependencies) / sizeof(kDependencies[0]), // dependencyCount
    ykkz000::bridge::kHostApiVersion,        // requiredHostApi
};

void OnGameContext(ykkz000::bridge::GameContextEvent event, void* context) {
  for (const ykkz000::plugin::Effect* effect : ykkz000::plugin::GetAllEffects()) {
    if (effect->on_context != nullptr) {
      effect->on_context(event, context);
    }
  }
}

int RegisterAll() {
  const auto effects = ykkz000::plugin::GetAllEffects();
  return ykkz000::plugin::Api()->register_effects(effects.data(),
                                                  static_cast<std::uint32_t>(effects.size()));
}

} // namespace

YKKZ000_PLUGIN_API const ykkz000::bridge::PluginManifest* GetPluginManifest() {
  return &kManifest;
}

YKKZ000_PLUGIN_API int GetPlugin(ykkz000::bridge::Host* host) {
  if (host == nullptr || host->apiVersion < ykkz000::bridge::kHostApiVersion ||
      host->registerEffectType == nullptr || host->engine == nullptr) {
    return 0;
  }
  const ykkz000::plugin::EffectTypeApi* effecttype =
      GetEffectTypeApi(ykkz000::plugin::kEffectTypeApiVersion);
  const ykkz000::plugin::PersistenceApi* persistence =
      GetPersistenceApi(ykkz000::plugin::kPersistenceApiVersion);
  if (effecttype == nullptr || persistence == nullptr) {
    return 0;
  }

  // Capture this consumer's host for the shared engine-access layer.
  effecttype->set_context(host);

  // Declare the persisted city variables before the first game context is created.
  if (persistence->declare_city_int_vector(ykkz000::plugin::kCityPercentVarName,
                                           ykkz000::civ6::kMaxYields) == 0 ||
      persistence->declare_city_int_vector(ykkz000::plugin::kCityPerSuzerainVarName,
                                           ykkz000::civ6::kMaxYields) == 0) {
    return 0;
  }
  // Register effects first: if this fails the loader unloads this module, and subscribing the
  // lifecycle callbacks afterwards would leave a dangling subscriber in the persistence plugin.
  if (RegisterAll() != 0) {
    return 0;
  }
  if (!ykkz000::plugin::RegisterObjectLifecycle()) {
    return 0;
  }

  host->onGameContext = &OnGameContext; // Register the context callback the Loader broadcasts through.
  return 1;
}

YKKZ000_PLUGIN_API void DestroyPlugin() {
  for (const ykkz000::plugin::Effect* effect : ykkz000::plugin::GetAllEffects()) {
    if (effect->shutdown != nullptr) {
      effect->shutdown();
    }
  }
  if (const ykkz000::plugin::EffectTypeApi* api =
          GetEffectTypeApi(ykkz000::plugin::kEffectTypeApiVersion);
      api != nullptr) {
    api->set_context(nullptr);
  }
}
