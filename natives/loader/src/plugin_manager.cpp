#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "loader_internal.h"

namespace ykkz000::loader {
namespace {

// 每个插件一个宿主视图（bridge::Host 副本）：pluginHandle 指向本结构，插件通过
// host->pluginHandle 回传归属；插件在 GetPlugin 里把 host->onGameContext 设为自身的
// 上下文回调，Loader 之后据此广播生命周期事件。
struct PluginSlot {
  HMODULE module = nullptr;
  bridge::DestroyPluginFn destroy = nullptr;
  bridge::ContextListenerFn listener = nullptr;
  bridge::Host host = {};
};

std::vector<PluginSlot*> g_plugins;
bool g_pluginsLoaded = false;

std::mutex g_activeMutex;
void* g_activePluginHandle = nullptr;

std::wstring normalized(const std::wstring& path) {
  wchar_t buffer[MAX_PATH] = {};
  const DWORD length = GetFullPathNameW(path.c_str(), MAX_PATH, buffer, nullptr);
  if (length == 0 || length >= MAX_PATH) {
    return path;
  }
  return std::wstring(buffer, length);
}

void addUnique(std::vector<std::wstring>& out, const std::wstring& path) {
  if (path.empty()) {
    return;
  }
  const std::wstring candidate = normalized(path);
  for (const std::wstring& existing : out) {
    if (existing == candidate) {
      return;
    }
  }
  out.push_back(candidate);
}

std::vector<std::wstring> pluginSearchDirs() {
  std::vector<std::wstring> dirs;
  const std::wstring base = moduleDirectory();
  if (base.empty()) {
    return dirs;
  }
  // 仅扫描专用的插件目录，绝不加载游戏 Binaries 目录中的任意 DLL。
  addUnique(dirs, base + L"\\ykkz000_civ6_plugin");
  addUnique(dirs, base + L"\\..\\..\\..\\..\\ykkz000_civ6_plugin");
  return dirs;
}

bool isSelfOrGameCore(HMODULE module) {
  if (module == g_selfModule) {
    return true;
  }
  return module == gameCore().module;
}

// POD-only 的 SEH 包裹：单个插件回调故障不拖垮游戏。VEH 见到 g_guardedCallActive
// 时放行给 __except，不写崩溃日志。
void callListenerGuarded(bridge::ContextListenerFn listener, bridge::GameContextEvent event,
                         void* context) {
  g_guardedCallActive = true;
  __try {
    listener(event, context);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // 忽略：单插件回调异常不应中断其它插件或引擎流程。
  }
  g_guardedCallActive = false;
}

void tryLoadPlugin(const std::wstring& file, const bridge::Host& hostTemplate) {
  LogScope scope("load plugin");
  logMessage(2, L"Trying plugin DLL: " + file);
  HMODULE module = LoadLibraryW(file.c_str());
  if (module == nullptr) {
    const DWORD error = GetLastError();
    wchar_t detail[512] = {};
    _snwprintf_s(detail, _countof(detail), _TRUNCATE,
                 L"Plugin LoadLibrary failed (GetLastError=%lu): %s", error, file.c_str());
    logMessage(1, detail);
    return;
  }
  if (isSelfOrGameCore(module)) {
    FreeLibrary(module); // 自身或 GameCore，静默跳过
    return;
  }
  logMessageF(1, "plugin: LoadLibrary -> %p", module);

  auto* getPlugin = reinterpret_cast<bridge::GetPluginFn>(
      GetProcAddress(module, YKKZ000_PLUGIN_EXPORT_GETPLUGIN));
  auto* destroy = reinterpret_cast<bridge::DestroyPluginFn>(
      GetProcAddress(module, YKKZ000_PLUGIN_EXPORT_DESTROY));
  logMessageF(1, "plugin: GetPlugin=%p DestroyPlugin=%p",
              reinterpret_cast<void*>(getPlugin), reinterpret_cast<void*>(destroy));
  if (getPlugin == nullptr) {
    logMessage(1, L"Skipped: GetPlugin not exported: " + file);
    FreeLibrary(module);
    return;
  }

  auto* slot = new (std::nothrow) PluginSlot();
  if (slot == nullptr) {
    logMessage(0, "plugin: failed to allocate plugin slot");
    FreeLibrary(module);
    return;
  }
  slot->module = module;
  slot->destroy = destroy;
  slot->host = hostTemplate;
  slot->host.pluginHandle = slot; // 每个插件独立的归属句柄

  setActivePluginHandle(slot);
  const int result = getPlugin(&slot->host);
  setActivePluginHandle(nullptr);
  logMessageF(1, "plugin: GetPlugin(host) -> %d", result);
  if (result <= 0) {
    // GetPlugin 失败：插件可能已登记 hook/槽/实现回调，必须先由 Loader 兜底还原
    // （撤销仍在登记表里的 plugin 归属 hook 与效果槽、清空实现回调），再让插件自行
    // 清理并卸载，否则指向插件代码的 hook/槽在 FreeLibrary 后即成悬垂指针。
    removeHooksForPlugin(slot);
    teardownPluginEffects(slot);
    if (destroy != nullptr) {
      destroy();
    }
    FreeLibrary(module);
    delete slot;
    wchar_t detail[512] = {};
    _snwprintf_s(detail, _countof(detail), _TRUNCATE,
                 L"Plugin init failed: GetPlugin returned %d; unloaded: %s", result,
                 file.c_str());
    logMessage(1, detail);
    return;
  }

  slot->listener = slot->host.onGameContext; // 插件在 GetPlugin 中登记
  g_plugins.push_back(slot);
  logMessage(1, L"Plugin loaded: " + file);
}

void scanDirectory(const std::wstring& directory, const bridge::Host& host) {
  LogScope scope("scan plugin directory");
  WIN32_FIND_DATAW data = {};
  const std::wstring pattern = directory + L"\\*.dll";
  HANDLE find = FindFirstFileW(pattern.c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) {
    logMessage(2, L"Plugin directory missing or has no readable DLL: " + directory);
    return;
  }
  logMessage(1, L"Scanning plugin directory: " + directory);
  do {
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      continue;
    }
    tryLoadPlugin(directory + L"\\" + data.cFileName, host);
  } while (FindNextFileW(find, &data) != FALSE);
  FindClose(find);
}

} // namespace

void loadPlugins(bridge::Host* host) {
  if (host == nullptr || g_pluginsLoaded) {
    return;
  }
  LogScope scope("load plugins (core)");
  g_pluginsLoaded = true;
  const std::vector<std::wstring> directories = pluginSearchDirs();
  logMessage(1, L"Plugin search directory count: " + std::to_wstring(directories.size()));
  for (const std::wstring& directory : directories) {
    scanDirectory(directory, *host);
  }
  logMessage(1, L"Plugin loading finished; loaded count: " + std::to_wstring(g_plugins.size()));
}

void setActivePluginHandle(void* pluginHandle) {
  std::lock_guard<std::mutex> guard(g_activeMutex);
  g_activePluginHandle = pluginHandle;
}

void* activePluginHandle() {
  std::lock_guard<std::mutex> guard(g_activeMutex);
  return g_activePluginHandle;
}

void notifyPluginsGameContext(bridge::GameContextEvent event, void* context) {
  for (PluginSlot* slot : g_plugins) {
    if (slot->listener == nullptr) {
      continue;
    }
    callListenerGuarded(slot->listener, event, context);
  }
}

void unloadPlugins() {
  LogScope scope("unload plugins");
  // 逆序卸载：与加载顺序相反，确保依赖关系被正确拆解。
  for (auto it = g_plugins.rbegin(); it != g_plugins.rend(); ++it) {
    PluginSlot* slot = *it;
    if (slot == nullptr) {
      continue;
    }
    // 先让插件清理自身（撤销 hook、清理缓存），再做 Loader 兜底。
    if (slot->listener != nullptr) {
      callListenerGuarded(slot->listener, bridge::GameContextEvent::kDestroyed, nullptr);
    }
    // 兜底：仍指向该插件代码的 hook 与 vtable 槽一律移除/还原，然后才允许卸载。
    removeHooksForPlugin(slot);
    teardownPluginEffects(slot);
    if (slot->destroy != nullptr) {
      slot->destroy();
    }
    if (slot->module != nullptr) {
      FreeLibrary(slot->module);
    }
    delete slot;
  }
  g_plugins.clear();
  g_pluginsLoaded = false;
}

} // namespace ykkz000::loader
