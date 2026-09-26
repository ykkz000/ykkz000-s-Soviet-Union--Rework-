#include <windows.h>

#include "loader_internal.h"

namespace ykkz000::loader {

HMODULE g_selfModule = nullptr;

} // namespace ykkz000::loader

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID /*reserved*/) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(instance);
    ykkz000::loader::g_selfModule = instance;
  }
  return TRUE;
}
