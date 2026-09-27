#include <windows.h>

#include <cstdio>

#include "loader_internal.h"

namespace ykkz000::loader {

HMODULE g_selfModule = nullptr;

} // namespace ykkz000::loader

namespace {

// DllMain 处于 loader lock，禁止文件 I/O；仅输出到调试器。
void logDebugOnly(const char* text) {
  char buffer[160] = {};
  _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "[YKKZ000] %s\n", text);
  OutputDebugStringA(buffer);
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID /*reserved*/) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
    ykkz000::loader::g_selfModule = instance;
    // pin 本模块：确保克隆 vtable / handler / VEH 在进程生命周期内始终有效，
    // 引擎关停阶段（Gameplay DLL 卸载/重载）不会因调用已卸载代码而崩溃。
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&ykkz000::loader::g_selfModule), &pinned);
    logDebugOnly("BEGIN: dll process attach");
    logDebugOnly("END: dll process attach");
  } else if (reason == DLL_PROCESS_DETACH) {
    logDebugOnly("BEGIN: dll process detach");
    // loader lock 下：仅移除 VEH，避免其指向已卸载代码（不记录日志/文件 I/O）。
    ykkz000::loader::uninstallCrashCapture();
    logDebugOnly("END: dll process detach");
  }
  return TRUE;
}
