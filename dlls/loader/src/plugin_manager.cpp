#include <windows.h>

#include <cstddef>
#include <string>
#include <vector>

#include "loader_internal.h"

namespace ykkz000::loader {
namespace {

struct LoadedPlugin {
  HMODULE module = nullptr;
  bridge::DestroyPluginFn destroy = nullptr;
};

std::vector<LoadedPlugin> g_plugins;
bool g_pluginsLoaded = false;

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

void tryLoadPlugin(const std::wstring& file, bridge::Host* host) {
  HMODULE module = LoadLibraryW(file.c_str());
  if (module == nullptr || isSelfOrGameCore(module)) {
    if (module != nullptr) {
      FreeLibrary(module);
    }
    return;
  }

  auto* getPlugin = reinterpret_cast<bridge::GetPluginFn>(
      GetProcAddress(module, YKKZ000_PLUGIN_EXPORT_GETPLUGIN));
  auto* destroy = reinterpret_cast<bridge::DestroyPluginFn>(
      GetProcAddress(module, YKKZ000_PLUGIN_EXPORT_DESTROY));

  if (getPlugin == nullptr) {
    FreeLibrary(module);
    return;
  }

  const int result = getPlugin(host);
  if (result <= 0) {
    if (destroy != nullptr) {
      destroy();
    }
    FreeLibrary(module);
    logMessage(1, "插件 GetPlugin 返回失败，已卸载");
    return;
  }

  g_plugins.push_back(LoadedPlugin{module, destroy});
  logMessage(1, "已加载插件");
}

void scanDirectory(const std::wstring& directory, bridge::Host* host) {
  WIN32_FIND_DATAW data = {};
  const std::wstring pattern = directory + L"\\*.dll";
  HANDLE find = FindFirstFileW(pattern.c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) {
    return;
  }
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
  g_pluginsLoaded = true;
  for (const std::wstring& directory : pluginSearchDirs()) {
    scanDirectory(directory, host);
  }
}

void unloadPlugins() {
  for (auto it = g_plugins.rbegin(); it != g_plugins.rend(); ++it) {
    if (it->destroy != nullptr) {
      it->destroy();
    }
    if (it->module != nullptr) {
      FreeLibrary(it->module);
    }
  }
  g_plugins.clear();
  g_pluginsLoaded = false;
}

} // namespace ykkz000::loader
