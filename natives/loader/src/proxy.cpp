#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
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

// —— 暴露给插件的受校验读写（bridge::Host 服务）——
int hostReadField(const void* base, std::size_t offset, std::size_t bytes, void* out) {
  if (base == nullptr || out == nullptr || bytes == 0) {
    return 0;
  }
  const auto* address = static_cast<const std::uint8_t*>(base) + offset;
  if (!isReadableRegion(address, bytes)) {
    return 0;
  }
  std::memcpy(out, address, bytes);
  return 1;
}

int hostWriteField(void* base, std::size_t offset, std::size_t bytes, const void* in) {
  if (base == nullptr || in == nullptr || bytes == 0) {
    return 0;
  }
  auto* address = static_cast<std::uint8_t*>(base) + offset;
  if (!isReadableRegion(address, bytes)) {
    return 0;
  }
  std::memcpy(address, in, bytes);
  return 1;
}

int hostIsCandidateObject(const void* pointer) {
  return isCandidateObject(pointer) ? 1 : 0;
}

int hostIsReadableRegion(const void* address, std::size_t bytes) {
  return isReadableRegion(address, bytes) ? 1 : 0;
}

bool initialize() {
  installCrashCapture();  // 尽早安装，保证之后任何崩溃都能记录
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

  // 效果 handler 注册：必须在真实 DllCreateGameContext 之前安装 hook，才能捕获
  // 游戏上下文构造期间建立的内建 handler 表并补登自定义效果。
  const bool handlerHookReady = installEffectHandlerHook();
  logMessageF(1, "effect handler hook ready=%d", handlerHookReady ? 1 : 0);

  {
    LogScope s("build host interface");
    g_host.apiVersion = bridge::kHostApiVersion;
    g_host.makeHash = &makeHash;
    g_host.registerEffectType = &registerEffectType;
    g_host.log = &logMessage;
    g_host.gameCoreModule = api.module;
    g_host.getEffectRegistry = reinterpret_cast<bridge::GetEffectRegistryFn>(api.getEffectRegistry);
    g_host.engine = &engineApi();
    g_host.installHook = &serviceInstallHook;
    g_host.removeHook = &serviceRemoveHook;
    g_host.patchEffectSlot = &hostPatchEffectSlot;
    g_host.readField = &hostReadField;
    g_host.writeField = &hostWriteField;
    g_host.isCandidateObject = &hostIsCandidateObject;
    g_host.isReadableRegion = &hostIsReadableRegion;
    g_host.onGameContext = nullptr; // 由各插件在自己的 Host 副本中登记
    g_host.pluginHandle = nullptr;  // 由每个插件的 Host 副本填入
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
  // 通知插件：新上下文建立。插件在此重新安装/启用自身的 hook 并清空随上下文失效
  // 的缓存；Loader 不再识别任何具体行为，故不再有 install*IfNeeded()。
  notifyPluginsGameContext(bridge::GameContextEvent::kCreated, nullptr);
  (void)installEffectHandlerHook();
  if (g_realCreate == nullptr) {
    return nullptr;
  }
  LogScope call("call real DllCreateGameContext");
  return g_realCreate();
}

void destroyGameContext(void* context) {
  LogScope scope("destroy game context");
  // 必须在真实 destroy 之前：此时 handler root 仍有效，可安全摘掉我们登记的节点，
  // 否则引擎关停阶段会调用到不成立的 handler 而崩溃（见退出崩溃分析）。
  removeCustomEffectHandlers();
  if (g_realDestroy != nullptr) {
    LogScope call("call real DllDestroyGameContext");
    g_realDestroy(context);
  }
  // 上下文已销毁：通知插件停用 hook、清理缓存。
  //
  // 注意：插件 DLL 保持加载，不在此处卸载。游戏在主菜单与对局之间会反复
  // create/destroy 上下文，而效果注册只发生一次（注册必须早于 ModifierLibrary::
  // Initialize），卸载后无法在下一个上下文重新注册。插件随进程一起存活；真正需要
  // 卸载时（进程退出）由 unloadPlugins() 按“先通知、再兜底还原、后 FreeLibrary”
  // 的顺序处理。
  notifyPluginsGameContext(bridge::GameContextEvent::kDestroyed, context);
  uninstallEffectHandlerHook();
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
