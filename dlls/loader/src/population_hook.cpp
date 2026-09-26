#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <mutex>

#include <MinHook.h>

#include "loader_internal.h"

// 人口变化补正 hook。
//
// 已应用的自定义效果在 Apply 时按“当时人口 × Amount”写入城市产出修正，而引擎的
// City::Instance::ChangePopulation 只更新人口、不重算修饰器，因此需要在本 hook 中
// 按实际人口增量对已登记效果追加 Amount × Δ，使累计恒等于 Amount × 当前人口。
//
// 注意：hook 内只调用 City::Instance::ChangeYieldModifier（纯增量并派发报告事件），
// 不触发修饰系统重算，避免递归。
namespace ykkz000::loader {
namespace {

using ChangePopulationFn = void (*)(void* city, int delta);

ChangePopulationFn g_originalChangePopulation = nullptr;
bool g_hookInstalled = false;
bool g_minHookInitialized = false;
std::mutex g_hookMutex;

int readPopulation(const void* city) {
  return *reinterpret_cast<const int*>(
      static_cast<const std::uint8_t*>(city) + kCityPopulationOffset);
}

// ChangePopulation 自带 `0 < population + delta` 的下限保护，可能不按 delta 生效；
// 因此以“调用前后的人口差”判断是否发生实际变化，再由 adjustAppliedCityPopulation
// 按各条目自身的人口基准校正到当前人口。
void ChangePopulation_Hook(void* city, int delta) {
  if (g_originalChangePopulation == nullptr) {
    return;
  }
  if (city == nullptr) {
    g_originalChangePopulation(city, delta);
    return;
  }
  const int before = readPopulation(city);
  g_originalChangePopulation(city, delta);
  if (readPopulation(city) != before) {
    adjustAppliedCityPopulation(city);
  }
}

} // namespace

bool installPopulationHook() {
  std::lock_guard<std::mutex> guard(g_hookMutex);
  LogScope scope("install population hook");
  if (g_hookInstalled) {
    return true;
  }
  const GameCoreApi& api = gameCore();
  if (api.changePopulation == nullptr) {
    logMessage(0, "Population hook: ChangePopulation entry unavailable; per-population "
                  "effects will fall back to a founding-time snapshot");
    return false;
  }
  if (!g_minHookInitialized) {
    const MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
      logMessage(0, "Population hook: MinHook initialization failed; per-population effects "
                    "will fall back to a founding-time snapshot");
      return false;
    }
    g_minHookInitialized = true;
  }
  if (g_originalChangePopulation == nullptr) {
    const MH_STATUS created = MH_CreateHook(
        api.changePopulation, reinterpret_cast<LPVOID>(&ChangePopulation_Hook),
        reinterpret_cast<LPVOID*>(&g_originalChangePopulation));
    if (created != MH_OK) {
      logMessage(0, "Population hook: MinHook CreateHook failed; per-population effects will "
                    "fall back to a founding-time snapshot");
      if (g_minHookInitialized) {
        MH_Uninitialize();
        g_minHookInitialized = false;
      }
      return false;
    }
  }
  const MH_STATUS enabled = MH_EnableHook(api.changePopulation);
  if (enabled != MH_OK) {
    logMessage(0, "Population hook: MinHook EnableHook failed; per-population effects will "
                  "fall back to a founding-time snapshot");
    return false;
  }
  g_hookInstalled = true;
  logMessage(1, "Population hook: ChangePopulation delta compensation installed");
  return true;
}

// 反安装只停用 hook，不释放 MinHook 跳板、也不重置 g_originalChangePopulation：
// 反安装发生在进程退出（DllDestroyGameContext），此时可能有线程仍执行在 detour 内，
// 释放跳板会导致其调用到已释放内存。跳板随进程结束一并回收，泄漏代价可忽略。
// 停用后由 installPopulationHook() 在下一个游戏上下文重新启用（同一模块保持映射）。
void uninstallPopulationHook() {
  std::lock_guard<std::mutex> guard(g_hookMutex);
  LogScope scope("uninstall population hook");
  if (g_hookInstalled) {
    const GameCoreApi& api = gameCore();
    if (api.changePopulation != nullptr) {
      MH_DisableHook(api.changePopulation);
    }
    g_hookInstalled = false;
  }
  clearAppliedCityModifiers();
}

} // namespace ykkz000::loader
