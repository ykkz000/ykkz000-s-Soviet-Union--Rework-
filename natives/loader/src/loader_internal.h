#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <ykkz000/bridge.h>
#include <ykkz000/civ6/common.h>
#include <ykkz000/civ6/effect.h>
#include <ykkz000/civ6/factory.h>
#include <ykkz000/civ6/handler.h>

namespace ykkz000::loader {

// —— 引擎侧布局 ——
// 布局的单一事实来源是 <ykkz000/civ6/*.h>：那里以 POD 数据成员 + static_assert
// 锁定每个已知偏移与大小，并以 vtable 槽位常量取代裸数字。调用点必须引用那里的
// 类型（offsetof / sizeof / 槽位常量），不得再写字段偏移字面量。
// 本命名空间只保留“机制常量”（克隆宽度、注册表 kind 等，均非行为策略）。

// 克隆宽度（机制常量，非布局）：效果对象 vtable 需覆盖 Apply/Remove 及析构槽，
// 64 槽远大于实际接口宽度，用于按函数指针定位并替换；handler 描述表取 16 槽留余量。
constexpr std::size_t kEffectVTableCloneSlots = 64;
constexpr std::size_t kHandlerTableCloneSlots = 16;

// 处理器注册表 kind：效果表（对应 civ6::HandlerRegistryRoot::effects）。
constexpr int kHandlerKindEffects = 2;

extern HMODULE g_selfModule;

// 引擎内部函数指针类型（仅本 DLL 内部使用）。
using GetGameManagerFn = void* (*)(); // FUN_180044d60() -> GameManager*

// 真实 GameCore 中被特征扫描解析出的内部入口。
struct GameCoreApi {
  HMODULE module = nullptr;
  void*   getEffectRegistry = nullptr; // Registry<IModifierEffectFactory>::GetTypes()
  void*   mallocTemp = nullptr;        // Platform::MallocTemp(size, file, line, a, b)
  void*   reserveVector = nullptr;     // std::vector::_Reserve(count) 成员函数
  void*   effectApply = nullptr;       // Effects::AdjustCityYieldModifier::Apply
  void*   effectRemove = nullptr;      // Effects::AdjustCityYieldModifier::Remove
  // 玩家单位战斗力修正模板（可选：缺失时“每宗主”行为退化为不缩放）。
  void*   effectStrengthApply = nullptr;  // Effects::AdjustPlayerStrengthModifier::Apply
  void*   effectStrengthRemove = nullptr; // Effects::AdjustPlayerStrengthModifier::Remove
  void*   getPlayerByIndex = nullptr;     // 保留：FUN_180044f00(int)（无边界检查，不用）
  void*   getGameManager = nullptr;       // FUN_180044d60() -> GameManager*
  void*   strengthAccumulate = nullptr;   // FUN_180944040(target, playerId, amount)：引擎把
                                          // 战斗力修正落到玩家桶的写入点；playerId 是引擎
                                          // 自己解析出的权威玩家标识
  void*   changeYieldModifier = nullptr; // City::Instance::ChangeYieldModifier(YieldTypes, int)
  void*   changePopulation = nullptr;  // City::Instance::ChangePopulation(int delta)
  // 处理器注册：handlerRegistryInit/setEffectHandler/handlerNodeInsert 为代码；后两者指向引擎数据。
  void*   handlerRegistryInit = nullptr;   // FUN_1804891b0(root)：建立内建 handler 表
  void*   setEffectHandler = nullptr;      // FUN_1806083f0(root, kind, hash, handlerObj)
  void*   handlerNodeInsert = nullptr;     // FUN_180489040(container, outNode, hashPtr)
  // 数据类 RVA：未经运行期校验，勿用于登记（保留仅作诊断）。各次运行间对不上，
  // 真实对象改由 handlerNodeInsert 在运行期从引擎注册表捕获（见 effect_handler.cpp）。
  void*   templateEffectHandler = nullptr; // EFFECT_ADJUST_CITY_YIELD_MODIFIER 的 handler 对象
  void*   templateHandlerTable = nullptr;  // 其描述表（克隆/替换 apply 用）
  // 模板描述表的两个槽（仅诊断用：确认自定义效果是否走到 handler 应用路径）：
  void*   templateAnalyze = nullptr;       // 槽 0：FUN_18046b0d0(self, args)
  void*   templateApply = nullptr;         // 槽 1：FUN_18046b390(self, context, args)
  // handler 派发 thunk（RVA 0x979290）：rcx=[rcx+0x18]; jmp [rax+0x30]。可选入口，
  // 缺失仅跳过对应 hook（诊断 + 无效 handler 守卫）。
  void*   effectHandlerDispatch = nullptr;
};

[[nodiscard]] std::wstring moduleDirectory();
[[nodiscard]] bool ensureGameCoreLoaded();
[[nodiscard]] const GameCoreApi& gameCore();
// 引擎入口集合（在 gameCore 解析成功后填充；未经解析时字段为 null）。
[[nodiscard]] const bridge::EngineApi& engineApi();

[[nodiscard]] std::uint32_t makeHash(const char* text);
void logMessage(int level, const char* message);
void logMessage(int level, const std::wstring& message);
void logMessageF(int level, const char* format, ...);

// 阶段日志：构造输出 "BEGIN: <stage>"，析构输出 "END: <stage>"。
class LogScope {
 public:
  explicit LogScope(const char* stage) : stage_(stage) {
    logMessage(1, (std::string("BEGIN: ") + stage_).c_str());
  }
  ~LogScope() { logMessage(1, (std::string("END: ") + stage_).c_str()); }
  LogScope(const LogScope&) = delete;
  LogScope& operator=(const LogScope&) = delete;

 private:
  const char* stage_;
};

// registry.cpp
[[nodiscard]] int registerEffectType(const bridge::EffectDesc* desc);

// 已成功注册的 EffectType 记录，供效果 handler 注册使用（typeName 为本副本，
// 不依赖已可能卸载的插件模块）。
struct RegisteredEffect {
  std::uint32_t hash = 0;
  std::uint32_t templateHash = 0;
  std::string typeName;
};
[[nodiscard]] std::vector<RegisteredEffect> registeredEffects();
// 判定给定哈希是否为某个已注册自定义效果所复用的模板效果哈希（供 handler
// 节点捕获时按模板分别记录）。
[[nodiscard]] bool isRegisteredTemplateHash(std::uint32_t hash);
// 判定给定哈希是否为某个已注册自定义效果本身的类型哈希（工厂对象 +0x08），用于在
// 模板 Create 旁路诊断中区分“引擎内置对象”与“我们克隆出的对象”。
[[nodiscard]] bool isRegisteredHash(std::uint32_t typeHash);

// hook_service.cpp：进程内唯一 MinHook 实例的初始化入口（幂等），供 Loader 自身
// 的机制 hook 与插件 hook 服务共用。
[[nodiscard]] bool ensureHookServiceInitialized();

// hook_service.cpp：Loaders 自身的机制 hook（无归属登记，不随插件卸载撤销）。
int installHookRaw(void* target, void* detour, void** original);
int removeHookRaw(void* target);

// hook_service.cpp：插件 hook 服务，按 pluginHandle 登记归属。
int serviceInstallHook(void* pluginHandle, void* target, void* detour, void** original);
int serviceRemoveHook(void* pluginHandle, void* target);
// 兜底撤销：移除该插件登记但尚未撤销的全部 hook。
void removeHooksForPlugin(void* pluginHandle);
// 调用期批次：registerEffectType 调用插件 prepare 前开启、调用后关闭；rollback 为真时
// 移除批次内新建的全部 hook（prepare 失败的兜底，主责仍是插件自行撤销）。
void beginHookScope();
void endHookScope(bool rollback);

// memory_probe.cpp
// 判定 [address, address+bytes) 是否落在已提交且可读的内存区域。
[[nodiscard]] bool isReadableRegion(const void* address, std::size_t bytes);
// 判定指针是否像一个可解引用的对象（非低地址、8 字节对齐、首指针可读）。
[[nodiscard]] bool isCandidateObject(const void* pointer);

// 只有通过 isReadableRegion 校验才读取字段；失败返回 false 且不改动 out。
template <typename T>
[[nodiscard]] bool tryReadField(const void* base, std::size_t offset, T& out) {
  if (base == nullptr) {
    return false;
  }
  const auto* address = static_cast<const std::uint8_t*>(base) + offset;
  if (!isReadableRegion(address, sizeof(T))) {
    return false;
  }
  std::memcpy(&out, address, sizeof(T));
  return true;
}

// —— 成员访问层：把“成员引用”翻译成偏移 ——
// 调用点用 (&civ6::X::field) 表达字段，偏移由 <ykkz000/civ6/*.h> 的布局决定；
// 引擎指针仍先经 isReadableRegion 校验，未通过则不改动内存。

// 由成员指针取字段偏移。以对齐的静态哑对象为基准取成员地址，避免对空指针取址；
// 全程只做地址相减，不读取任何成员。
template <class TObj, class TField>
[[nodiscard]] std::size_t MemberOffset(TField TObj::* member) {
  static const TObj kDummy{};
  const auto base = reinterpret_cast<std::uintptr_t>(&kDummy);
  const auto field = reinterpret_cast<std::uintptr_t>(&(kDummy.*member));
  return static_cast<std::size_t>(field - base);
}

// 校验可读后读取成员字段；失败返回 false 且不改动 out。
template <class TObj, class TField>
[[nodiscard]] bool TryRead(const void* base, TField TObj::* member, TField& out) {
  return tryReadField(base, MemberOffset(member), out);
}

// 校验可读后读取成员字段，失败返回 fallback。
template <class TObj, class TField>
[[nodiscard]] TField TryReadOr(const void* base, TField TObj::* member, TField fallback) {
  TField value = fallback;
  (void)TryRead(base, member, value);
  return value;
}

// 校验可读后写入成员字段；失败返回 false 且不改动内存。
template <class TObj, class TField>
bool TryWrite(void* base, TField TObj::* member, const TField& value) {
  if (base == nullptr) {
    return false;
  }
  auto* address = static_cast<std::uint8_t*>(base) + MemberOffset(member);
  if (!isReadableRegion(address, sizeof(TField))) {
    return false;
  }
  std::memcpy(address, &value, sizeof(TField));
  return true;
}

// crash_capture.cpp：安装崩溃现场抓取（VEH）；幂等。
void installCrashCapture();
// 反安装崩溃抓取：移除 VEH，避免 DLL 卸载后 VEH 指向已卸载代码。
void uninstallCrashCapture();

// crash_capture.cpp：本线程是否正在执行一次受 SEH 保护的引擎/插件调用。VEH 看到该
// 标志时直接放行（EXCEPTION_CONTINUE_SEARCH），让受保护调用的 __except 接管，避免把
// 可恢复的探测性异常当成致命崩溃写进 YKKZ000_crash.log。
extern thread_local bool g_guardedCallActive;

// effect_handler.cpp
[[nodiscard]] bool installEffectHandlerHook();
void uninstallEffectHandlerHook();
// 移除本模组登记过的 handler 节点（在转发真实 DllDestroyGameContext 之前调用，
// 此时 root 仍有效）。请勿在其它时机调用。
void removeCustomEffectHandlers();
// 卸载兜底：清空该插件在 handler 克隆表里登记的 analyze/handlerApply 回调，
// 避免插件卸载后仍有 handler 调用跳进已卸载内存。
void clearHandlerCallbacksForPlugin(void* pluginHandle);

// vtable_clone.cpp
[[nodiscard]] void* cloneFactoryVTable(void* templateFactory);
void rememberTypeName(std::uint32_t typeHash, const char* typeName);
// 注册失败回滚：移除某 hash 的类型名记录（GetTypeName 槽随后返回空串）。
void forgetTypeName(std::uint32_t typeHash);

// effect_mechanism.cpp
// 已登记的插件实现（impl 为拷贝，含归属句柄；originalCreate 为模板工厂 Create）。
struct EffectRecord {
  bridge::EffectImpl impl{};
  void* originalCreate = nullptr;
  void* pluginHandle = nullptr;
};
// 按自定义 EffectType 哈希取实现记录（不存在返回 false）。
[[nodiscard]] bool findEffectRecord(std::uint32_t typeHash, EffectRecord& out);
// 登记/刷新某 EffectType 的实现与模板 Create（同一哈希重复调用为刷新）。
int registerEffectImpl(std::uint32_t typeHash, const bridge::EffectImpl* impl,
                       void* originalCreate);
// 注册失败回滚：移除某 EffectType 的实现记录（不存在返回非 0）。
int unregisterEffectImpl(std::uint32_t typeHash);
// 克隆效果对象 vtable 并按记录里的 template/impl 函数指针替换 Apply/Remove 槽。
[[nodiscard]] void* patchEffectObjectSlots(void* effectObject, std::uint32_t typeHash);
[[nodiscard]] void* customFactoryCreateEntry();
// 插件可选的单槽替换服务（bridge::SlotPatchFn 实现）：仅在 Loader 已安装的克隆块
// 上就地替换，拒绝修改引擎共享的静态 vtable。
int hostPatchEffectSlot(void* pluginHandle, void* object, const void* expectedFn,
                        void* replacement, const char* label);
// 卸载兜底：把该插件替换过的效果对象槽还原为模板函数，并清空其实现回调指针。
void teardownPluginEffects(void* pluginHandle);

// plugin_manager.cpp
void loadPlugins(bridge::Host* host);
void unloadPlugins();
// 当前正在加载的插件句柄（仅在 GetPlugin 调用期间设置，供注册归属使用）。
void setActivePluginHandle(void* pluginHandle);
[[nodiscard]] void* activePluginHandle();
// 向所有已加载插件广播上下文生命周期事件。
void notifyPluginsGameContext(bridge::GameContextEvent event, void* context);

// proxy.cpp
void initializeLoaderOnce();
[[nodiscard]] bool loaderInitialized();
[[nodiscard]] void* createGameContext();
void destroyGameContext(void* context);
[[nodiscard]] std::uint64_t telemetrySessionHash();

} // namespace ykkz000::loader
