#pragma once

#include <cstddef>
#include <cstdint>

// 宿主（Loader）与插件之间的纯 C ABI 边界。
// 仅使用 POD、函数指针与 const char*；不跨 DLL 传递 std:: 对象或异常。
namespace ykkz000::bridge {

inline constexpr std::uint32_t kHostApiVersion = 6;

struct Host;

using MakeHashFn          = std::uint32_t (*)(const char*);
using LogFn               = void (*)(int level, const char* msg);
using GetEffectRegistryFn = void* (*)();

// 效果对象 vtable 的 Apply/Remove 槽：引擎按模板真实签名传参，这里统一以 4 个指针
// 实参接收（x64 下多收参数无损），转发时按同样实参调用。
using ApplyFn = std::uint64_t (*)(void* self, void* a1, void* a2, void* a3);
// handler 描述表槽 0（analyze）与槽 1（apply）的签名。
using AnalyzeFn = void* (*)(void* self, void* args);

// 插件为某个自定义 EffectType 提供的“实现”。均为可选：为 null 的槽沿用模板行为，
// Loader 完全泛化处理，不再识别任何具体行为。
struct EffectImpl {
  // 引擎模板 Apply/Remove 函数指针：Loader 用它（按函数指针值）在克隆 vtable 里
  // 定位需要替换的槽。当 apply/remove 非 null 时必须提供。
  const void* templateApply;
  const void* templateRemove;
  ApplyFn     apply;        // 替换效果对象 Apply 槽；null = 沿用模板
  ApplyFn     remove;       // 替换效果对象 Remove 槽；null = 沿用模板
  AnalyzeFn   analyze;      // 替换 handler 槽 0；null = 转发模板
  ApplyFn     handlerApply; // 替换 handler 槽 1；null = 转发模板
  const char* label;        // 诊断名（可空）
  void*       userData;     // 插件上下文，Loader 原样保留，不解释
};

// 注意：GameEffects 元数据由引擎依模板的 GetTypeInfo 写入，
// 自定义元数据在当前机制下不受支持，故此处不提供相应字段。

// 准备入口（hook 安装函数）。契约：
//  * 由 host->registerEffectType 在“工厂/类型已登记之后”调用一次（注册期）。
//  * 返回 0 = 成功；非 0 = 失败 ⇒ registerEffectType 回滚本次注册（工厂对象/类型/
//    impl 记录）并返回错误码；prepare 自身必须撤销本次已安装的 hook（loader 不做
//    半程回滚）。
//  * 可为 null（该效果不需要 hook）。
//  * 与上下文相关的重装/停用不在这里做：仍走 Host::onGameContext（kCreated/
//    kDestroyed）与 DestroyPlugin（插件侧自行 Shutdown）。
using EffectPrepareFn = int (*)(void* userData);

struct EffectDesc {
  const char* typeName;       // 必填
  const char* templateEffect; // 必填，复用其行为与参数定义的已有效果名
  const EffectImpl* impl;     // null = 完全复用模板行为
  EffectPrepareFn prepare;    // 由 registerEffectType 调用；null = 无需 hook
  void* userData;             // 原样传给 prepare
};

using RegisterEffectTypeFn = int (*)(const EffectDesc*);

// 引擎入口：只读函数指针集合，按“只追加”策略演进（旧字段永不改含义）。
struct EngineApi {
  void* effectApply;          // Effects::AdjustCityYieldModifier::Apply/Remove
  void* effectRemove;
  void* effectStrengthApply;  // Effects::AdjustPlayerStrengthModifier::Apply/Remove
  void* effectStrengthRemove;
  void* proposedCombatAdjust; // 战斗力修正写入点（playerId 权威来源）
  void* changeYieldModifier;  // City::Instance::ChangeYieldModifier(YieldType, int)
  void* changePopulation;     // City::Instance::ChangePopulation(int delta)
  void* getPlayer;            // PlayerTypes → Player::Instance*（无边界检查）
  void* getGameManager;       // → GameManager*（+0x50 为玩家向量）
};

// MinHook 服务：全进程唯一实例由 Loader 的 hook_service 持有。插件禁止自行链接
// MinHook（两份实例会互相破坏 trampoline），一律经此安装/移除。
//
// 幂等契约：installHook 可在注册期与每次游戏上下文创建时重复调用。
//   - 返回 0 表示“目标已由本插件接管且可用”，包含“此前已安装”的重复调用；重复调用
//     会把既有跳板写回 original，插件可据此判断钩子仍然有效。
//   - 同一目标换用不同 detour 会被拒装（非 0）。
// removeHook 返回 0 表示“目标已不被本插件接管”，包含“此前已停用/未登记”的情况。
// “已安装/已停用”属成功语义，绝不能作为失败处理——否则插件会清空仍然生效的跳板。
using HookInstallFn = int (*)(void* pluginHandle, void* target, void* detour, void** original);
using HookRemoveFn  = int (*)(void* pluginHandle, void* target);

// 效果对象 vtable 槽替换（克隆块由 Loader 持有并登记归属）。返回 0 表示成功。
using SlotPatchFn = int (*)(void* pluginHandle, void* object, const void* expectedFn,
                            void* replacement, const char* label);

// 受校验的内存读写：以“偏移 + 字节数”表达，避免跨 DLL 传递类型信息。
// 成功返回 1，失败返回 0 且不改动 out/in。
using ReadFieldFn  = int (*)(const void* base, std::size_t offset, std::size_t bytes, void* out);
using WriteFieldFn = int (*)(void* base, std::size_t offset, std::size_t bytes, const void* in);

using IsCandidateFn = int (*)(const void* pointer);
using IsReadableFn  = int (*)(const void* address, std::size_t bytes);

// 游戏上下文生命周期事件。
enum class GameContextEvent : std::int32_t {
  kCreated = 0,
  kDestroyed = 1,
};

// 上下文生命周期通知：插件在 kCreated 时安装并启用自身 hook、清空随上下文失效的
// 缓存；在 kDestroyed 时停用 hook、清理缓存。回调由 Loader 用 SEH 包裹。
using ContextListenerFn = void (*)(GameContextEvent event, void* context);

struct Host {
  std::uint32_t        apiVersion;
  MakeHashFn           makeHash;
  RegisterEffectTypeFn registerEffectType;
  LogFn                log;
  void*                gameCoreModule;
  GetEffectRegistryFn  getEffectRegistry;

  // —— v5 新增（只追加）——
  const EngineApi*     engine;
  HookInstallFn        installHook;
  HookRemoveFn         removeHook;
  SlotPatchFn          patchEffectSlot;
  ReadFieldFn          readField;
  WriteFieldFn         writeField;
  IsCandidateFn        isCandidateObject;
  IsReadableFn         isReadableRegion;
  ContextListenerFn    onGameContext;
  void*                pluginHandle; // Loader 为每个插件分配的归属句柄
};

using GetPluginFn     = int  (*)(Host* host);
using DestroyPluginFn = void (*)();

} // namespace ykkz000::bridge

#define YKKZ000_PLUGIN_EXPORT_GETPLUGIN  "GetPlugin"
#define YKKZ000_PLUGIN_EXPORT_DESTROY    "DestroyPlugin"
