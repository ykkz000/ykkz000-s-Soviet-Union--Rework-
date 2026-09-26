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
    logDebugOnly("BEGIN: dll process attach");
    logDebugOnly("END: dll process attach");
  } else if (reason == DLL_PROCESS_DETACH) {
    logDebugOnly("BEGIN: dll process detach");
    logDebugOnly("END: dll process detach");
  }
  return TRUE;
}
