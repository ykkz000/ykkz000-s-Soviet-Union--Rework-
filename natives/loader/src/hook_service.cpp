#include <windows.h>

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <MinHook.h>

#include "loader_internal.h"

// 进程内唯一的 MinHook 门面（hook service）。
//
// 背景：MinHook 是全局单例；若插件自行链接一份 MinHook，两份状态会互相破坏
// trampoline。因此插件的所有 hook 一律经本服务安装，并按 pluginHandle 登记归属，
// 卸载时由 Loader 兜底撤销。
//
// 归属策略：
//   - Loader 自身的机制 hook（pluginHandle == nullptr）：只停用不移除，跳板保留在
//     Loader（常驻）内，便于下一个游戏上下文重新启用。
//   - 插件 hook：登记 pluginHandle；removeHooksForPlugin 在插件卸载时停用并移除，
//     避免 detour 指向已卸载代码。
//
// 幂等契约（重要）：installHook / removeHook 可在“注册期 + 每次游戏上下文创建”重复
// 调用。MinHook 的 MH_ERROR_ENABLED / MH_ERROR_DISABLED 表示“目标已处于期望状态”，
// 属成功语义，绝不可作为失败上报——否则插件会把已启用的 hook 视作安装失败并清空跳板，
// 留下“仍在拦截但不再转发”的静默丢弃状态。同一目标重复安装须复用既有跳板并写回
// 调用方的 original。
namespace ykkz000::loader {
namespace {

struct HookRecord {
  void* detour = nullptr;
  void** original = nullptr;
  void* pluginHandle = nullptr;
};

std::mutex g_hookMutex;
std::unordered_map<void*, HookRecord> g_hooks;

// —— 调用期批次（prepare 失败时的兜底回滚）——
// registerEffectType 在调用插件 prepare 前开启一个批次，批次内“新创建”的 hook 会被
// 登记；prepare 返回非 0 时 endHookScope(true) 一键移除本批次新建的全部 hook。
// 主责仍是插件在 prepare 失败时自行撤销；本批次只按“本次调用期间新建”兜底，不触碰
// 批次开启前已存在的 hook（重新启用场景不在此列）。
std::vector<std::vector<void*>> g_hookScopes;

std::once_flag g_initOnce;
bool g_initOk = false;

} // namespace

bool ensureHookServiceInitialized() {
  std::call_once(g_initOnce, []() {
    const MH_STATUS status = MH_Initialize();
    logMessageF(1, "hook: MH_Initialize -> %d", static_cast<int>(status));
    g_initOk = (status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED);
    if (!g_initOk) {
      logMessage(0, "hook: MinHook initialization failed; all hooks unavailable");
    }
  });
  return g_initOk;
}

int installHookRaw(void* target, void* detour, void** original) {
  return serviceInstallHook(nullptr, target, detour, original);
}

int removeHookRaw(void* target) {
  return serviceRemoveHook(nullptr, target);
}

int serviceInstallHook(void* pluginHandle, void* target, void* detour, void** original) {
  if (target == nullptr || detour == nullptr || original == nullptr) {
    return -1;
  }
  if (!ensureHookServiceInitialized()) {
    return -2;
  }
  std::lock_guard<std::mutex> guard(g_hookMutex);
  const auto it = g_hooks.find(target);
  if (it == g_hooks.end()) {
    void* trampoline = nullptr;
    const MH_STATUS created = MH_CreateHook(target, detour, &trampoline);
    logMessageF(1, "hook: create target=%p detour=%p -> %d trampoline=%p", target, detour,
                static_cast<int>(created), trampoline);
    if (created == MH_ERROR_ALREADY_CREATED) {
      // MinHook 已存在该目标的 hook，但本服务无登记：无法取回既有跳板、也无法比对
      // detour，故按“不同 detour”拒装。若强行登记将得到“已启用但跳板缺失”的危险状态。
      logMessageF(0, "hook: target=%p already created outside the hook service; refusing", target);
      return -3;
    }
    if (created != MH_OK) {
      return static_cast<int>(created);
    }
    HookRecord record;
    record.detour = detour;
    record.original = original;
    record.pluginHandle = pluginHandle;
    if (trampoline != nullptr) {
      *original = trampoline;
    }
    g_hooks.emplace(target, record);
    if (!g_hookScopes.empty()) {
      g_hookScopes.back().push_back(target); // 供 prepare 失败时回滚
    }
  } else {
    // 已登记：同一 detour 视为幂等复用，不同 detour 拒装（保持“一个目标一个 detour”）。
    HookRecord& record = it->second;
    if (record.detour != detour) {
      logMessageF(0, "hook: target=%p already hooked with a different detour; refusing", target);
      return -3;
    }
    if (record.original != nullptr && *record.original != nullptr) {
      *original = *record.original; // 复用既有跳板（重新启用场景）
      logMessageF(1, "hook: reuse target=%p trampoline=%p", target, *record.original);
    }
    if (pluginHandle != nullptr && record.pluginHandle == nullptr) {
      record.pluginHandle = pluginHandle; // 归属升级为插件
    }
  }
  const MH_STATUS enabled = MH_EnableHook(target);
  logMessageF(1, "hook: enable target=%p -> %d%s", target, static_cast<int>(enabled),
              enabled == MH_ERROR_ENABLED ? " (already enabled)" : "");
  if (enabled != MH_OK && enabled != MH_ERROR_ENABLED) {
    return static_cast<int>(enabled);
  }
  return 0;
}

int serviceRemoveHook(void* pluginHandle, void* target) {
  if (target == nullptr) {
    return -1;
  }
  std::lock_guard<std::mutex> guard(g_hookMutex);
  const auto it = g_hooks.find(target);
  if (it == g_hooks.end()) {
    return 0;
  }
  if (pluginHandle != nullptr && it->second.pluginHandle != nullptr &&
      it->second.pluginHandle != pluginHandle) {
    logMessageF(0, "hook: target=%p belongs to another plugin; refusing removal", target);
    return -1;
  }
  const MH_STATUS disabled = MH_DisableHook(target);
  logMessageF(1, "hook: disable target=%p -> %d%s", target, static_cast<int>(disabled),
              disabled == MH_ERROR_DISABLED ? " (already disabled)" : "");
  if (disabled != MH_OK && disabled != MH_ERROR_DISABLED) {
    return static_cast<int>(disabled);
  }
  return 0;
}

void beginHookScope() {
  std::lock_guard<std::mutex> guard(g_hookMutex);
  g_hookScopes.emplace_back();
}

void endHookScope(bool rollback) {
  std::vector<void*> targets;
  {
    std::lock_guard<std::mutex> guard(g_hookMutex);
    if (g_hookScopes.empty()) {
      return;
    }
    targets = std::move(g_hookScopes.back());
    g_hookScopes.pop_back();
    if (!rollback) {
      return;
    }
    for (void* target : targets) {
      MH_DisableHook(target);
      MH_RemoveHook(target);
      g_hooks.erase(target);
    }
  }
  for (void* target : targets) {
    logMessageF(1, "hook: scope rollback removed target=%p", target);
  }
}

void removeHooksForPlugin(void* pluginHandle) {
  if (pluginHandle == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> guard(g_hookMutex);
  int removed = 0;
  for (auto it = g_hooks.begin(); it != g_hooks.end();) {
    if (it->second.pluginHandle != pluginHandle) {
      ++it;
      continue;
    }
    const void* target = it->first;
    MH_DisableHook(it->first);
    MH_RemoveHook(it->first);
    it = g_hooks.erase(it);
    ++removed;
    logMessageF(1, "hook: removed plugin hook target=%p", target);
  }
  if (removed > 0) {
    logMessageF(1, "hook: removed %d hook(s) for plugin %p", removed, pluginHandle);
  }
}

} // namespace ykkz000::loader
