#include <ykkz000/bridge/host.h>
#include <ykkz000/export.h>
#include <ykkz000/plugin/effecttype_api.h>

// Entry points of the ykkz000_plugin_api_effecttype plugin. The loader initializes this plugin (as
// a manifest-declared dependency) before its consumers, so that when a consumer's GetPlugin runs the
// API is ready and its GetEffectTypeApi export is usable.
namespace {

constexpr ykkz000::bridge::PluginManifest kManifest = {
    sizeof(ykkz000::bridge::PluginManifest), // structSize
    ykkz000::bridge::kHostApiVersion,        // apiVersion
    "ykkz000.api.effecttype",                // name
    1,                                       // versionMajor
    0,                                       // versionMinor
    0,                                       // versionPatch
    nullptr,                                 // dependencies
    0,                                       // dependencyCount
    ykkz000::bridge::kHostApiVersion,        // requiredHostApi
};

} // namespace

YKKZ000_PLUGIN_API const ykkz000::bridge::PluginManifest* GetPluginManifest() { return &kManifest; }

YKKZ000_PLUGIN_API int GetPlugin(ykkz000::bridge::Host* host) {
  if (host == nullptr || host->apiVersion < ykkz000::bridge::kHostApiVersion) {
    return 0;
  }
  ykkz000::plugin::Api()->set_context(host);
  return 1;
}

YKKZ000_PLUGIN_API void DestroyPlugin() {
  if (const ykkz000::plugin::EffectTypeApi* api = ykkz000::plugin::Api(); api != nullptr) {
    api->set_context(nullptr);
  }
}
