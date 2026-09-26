#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <mutex>

#include "loader_internal.h"

namespace ykkz000::loader {
namespace {

using CreateGameContextFn = void* (*)();
using DestroyGameContextFn = void (*)(void*);
using TelemetryHashFn = std::uint64_t (*)();

std::once_flag g_initOnce;
bool g_initialized = false;
CreateGameContextFn g_realCreate = nullptr;
DestroyGameContextFn g_realDestroy = nullptr;
TelemetryHashFn g_realTelemetryHash = nullptr;
bridge::Host g_host = {};

bool initialize() {
  LogScope scope("initialize");
  {
    LogScope s("resolve real GameCore");
    if (!ensureGameCoreLoaded()) {
      logMessage(0, "Initialization failed: real GameCore unavailable");
      return false;
    }
  }

  const GameCoreApi& api = gameCore();
  {
    LogScope s("resolve GameCore exports");
    g_realCreate = reinterpret_cast<CreateGameContextFn>(
        GetProcAddress(api.module, "DllCreateGameContext"));
    g_realDestroy = reinterpret_cast<DestroyGameContextFn>(
        GetProcAddress(api.module, "DllDestroyGameContext"));
    g_realTelemetryHash = reinterpret_cast<TelemetryHashFn>(
        GetProcAddress(api.module, "EXP_GetTelemetrySessionHash"));
  }
  if (g_realCreate == nullptr) {
    logMessage(0, "Initialization failed: could not resolve real DllCreateGameContext");
    return false;
  }

  {
    LogScope s("build host interface");
    g_host.apiVersion = bridge::kHostApiVersion;
    g_host.makeHash = &makeHash;
    g_host.registerEffectType = &registerEffectType;
    g_host.log = &logMessage;
    g_host.gameCoreModule = api.module;
    g_host.getEffectRegistry = reinterpret_cast<bridge::GetEffectRegistryFn>(api.getEffectRegistry);
  }

  // 必须在真实 DllCreateGameContext 之前完成注册：其后会触发
  // ModifierLibrary::Initialize 与 DatabaseWriter::WriteEffectData。
  {
    LogScope s("load plugins");
    loadPlugins(&g_host);
  }
  return true;
}

} // namespace

void initializeLoaderOnce() {
  LogScope scope("initialize loader");
  std::call_once(g_initOnce, []() { g_initialized = initialize(); });
}

bool loaderInitialized() {
  return g_initialized;
}

void* createGameContext() {
  LogScope scope("create game context");
  initializeLoaderOnce();
  // 注册只发生一次，而 destroyGameContext 会停用 hook；按已注册行为重新启用。
  installPopulationHookIfNeeded();
  if (g_realCreate == nullptr) {
    return nullptr;
  }
  LogScope call("call real DllCreateGameContext");
  return g_realCreate();
}

void destroyGameContext(void* context) {
  LogScope scope("destroy game context");
  if (g_realDestroy != nullptr) {
    LogScope call("call real DllDestroyGameContext");
    g_realDestroy(context);
  }
  uninstallPopulationHook();
  unloadPlugins();
}

std::uint64_t telemetrySessionHash() {
  LogScope scope("telemetry session hash");
  initializeLoaderOnce();
  if (g_realTelemetryHash == nullptr && ensureGameCoreLoaded()) {
    g_realTelemetryHash = reinterpret_cast<TelemetryHashFn>(
        GetProcAddress(gameCore().module, "EXP_GetTelemetrySessionHash"));
  }
  return g_realTelemetryHash != nullptr ? g_realTelemetryHash() : 0;
}

} // namespace ykkz000::loader

extern "C" void* DllCreateGameContext() {
  return ykkz000::loader::createGameContext();
}

extern "C" void DllDestroyGameContext(void* context) {
  ykkz000::loader::destroyGameContext(context);
}

extern "C" int InitLoader() {
  ykkz000::loader::initializeLoaderOnce();
  return ykkz000::loader::loaderInitialized() ? 1 : 0;
}

extern "C" std::uint64_t EXP_GetTelemetrySessionHash() {
  return ykkz000::loader::telemetrySessionHash();
}
