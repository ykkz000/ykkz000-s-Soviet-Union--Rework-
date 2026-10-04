#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include <ykkz000/loader/internal.h>

namespace ykkz000::loader {
namespace {

// One host view per plugin (a copy of bridge::Host): pluginHandle points at this structure and the
// plugin hands the ownership back through host->pluginHandle; the plugin sets host->onGameContext
// to its own context callback in GetPlugin, and the Loader then broadcasts lifecycle events
// through it.
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
  // Scan only the dedicated plugin directories; never load arbitrary DLLs from the game Binaries
  // directory.
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

// POD-only SEH wrapper: a single plugin callback failure does not take down the game. When the VEH
// sees g_guardedCallActive it passes through to __except and writes no crash log.
void callListenerGuarded(bridge::ContextListenerFn listener, bridge::GameContextEvent event,
                         void* context) {
  g_guardedCallActive = true;
  __try {
    listener(event, context);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // Ignored: a single plugin callback exception must not interrupt other plugins or the engine.
  }
  g_guardedCallActive = false;
}

void tryLoadPlugin(const std::wstring& file, const bridge::Host& hostTemplate) {
  LogScope scope("load plugin");
  logDebug(L"Trying plugin DLL: " + file);
  HMODULE module = LoadLibraryW(file.c_str());
  if (module == nullptr) {
    const DWORD error = GetLastError();
    wchar_t detail[512] = {};
    _snwprintf_s(detail, _countof(detail), _TRUNCATE,
                 L"Plugin LoadLibrary failed (GetLastError=%lu): %s", error, file.c_str());
    logError(detail);
    return;
  }
  if (isSelfOrGameCore(module)) {
    FreeLibrary(module); // self or GameCore; skip silently
    return;
  }
  logDebugF("plugin: LoadLibrary -> %p", module);

  auto* getPlugin = reinterpret_cast<bridge::GetPluginFn>(
      GetProcAddress(module, YKKZ000_PLUGIN_EXPORT_GETPLUGIN));
  auto* destroy = reinterpret_cast<bridge::DestroyPluginFn>(
      GetProcAddress(module, YKKZ000_PLUGIN_EXPORT_DESTROY));
  logDebugF("plugin: GetPlugin=%p DestroyPlugin=%p",
            reinterpret_cast<void*>(getPlugin), reinterpret_cast<void*>(destroy));
  if (getPlugin == nullptr) {
    logWarn(L"Skipped: GetPlugin not exported: " + file);
    FreeLibrary(module);
    return;
  }

  auto* slot = new (std::nothrow) PluginSlot();
  if (slot == nullptr) {
    logError("plugin: failed to allocate plugin slot");
    FreeLibrary(module);
    return;
  }
  slot->module = module;
  slot->destroy = destroy;
  slot->host = hostTemplate;
  slot->host.pluginHandle = slot; // per-plugin ownership handle

  setActivePluginHandle(slot);
  const int result = getPlugin(&slot->host);
  setActivePluginHandle(nullptr);
  logInfoF("plugin: GetPlugin(host) -> %d", result);
  if (result <= 0) {
    // GetPlugin failed: the plugin may already have registered hooks/slots/implementation
    // callbacks, so the Loader must first restore as a fallback (revoke the still-registered
    // plugin-owned hooks and effect slots, clear implementation callbacks), and only then let the
    // plugin clean up and unload; otherwise hooks/slots pointing at plugin code become dangling
    // after FreeLibrary.
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
    logError(detail);
    return;
  }

  slot->listener = slot->host.onGameContext; // the plugin registers this inside GetPlugin
  g_plugins.push_back(slot);
  logInfo(L"Plugin loaded: " + file);
}

void scanDirectory(const std::wstring& directory, const bridge::Host& host) {
  LogScope scope("scan plugin directory");
  WIN32_FIND_DATAW data = {};
  const std::wstring pattern = directory + L"\\*.dll";
  HANDLE find = FindFirstFileW(pattern.c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) {
    logDebug(L"Plugin directory missing or has no readable DLL: " + directory);
    return;
  }
  logInfo(L"Scanning plugin directory: " + directory);
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
  logInfo(L"Plugin search directory count: " + std::to_wstring(directories.size()));
  for (const std::wstring& directory : directories) {
    scanDirectory(directory, *host);
  }
  logInfo(L"Plugin loading finished; loaded count: " + std::to_wstring(g_plugins.size()));
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
  // Unload in reverse order: opposite to load order, ensuring dependencies are torn down correctly.
  for (auto it = g_plugins.rbegin(); it != g_plugins.rend(); ++it) {
    PluginSlot* slot = *it;
    if (slot == nullptr) {
      continue;
    }
    // Let the plugin clean up first (revoke hooks, clear caches), then do the Loader fallback.
    if (slot->listener != nullptr) {
      callListenerGuarded(slot->listener, bridge::GameContextEvent::kDestroyed, nullptr);
    }
    // Fallback: remove/restore any hooks and vtable slots still pointing at this plugin's code
    // before unloading is allowed.
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
