#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <ykkz000/bridge.h>

namespace ykkz000::loader {

// —— 引擎侧布局（由 GameCore_XP2_FinalRelease.dll 逆向确认），单一事实来源 ——
// IModifierEffectFactory 工厂对象：+0x00 vtable，+0x08 uint32 类型哈希。
constexpr std::size_t kFactoryObjectSize = 0x10;
constexpr std::size_t kFactoryHashOffset = 0x08;
// 引擎 IModifierEffectFactory 接口宽于 6 槽：实测会在 +0x30（槽 6）上分发。
// 克隆必须“足够宽且完整复制”，绝不能只复制 6 个，否则越界读到堆垃圾并跳到 0xFFFFFFFF。
constexpr std::size_t kFactoryVTableSlots = 24;
constexpr std::size_t kFactoryTypeIdSlot = 1;
constexpr std::size_t kFactoryTypeNameSlot = 2;
constexpr std::size_t kFactoryCreateSlot = 5;

// MSVC vtable 前置槽：vtable[-1] 为 RTTI/COL 指针，克隆时必须一并复制，
// 否则新 vptr 前方是 HeapAlloc 块头，引擎 dynamic_cast/typeid/异常展开会把
// 堆头当指针用而写坏内存。
constexpr std::size_t kVTableRttiSlots = 1;

// 效果对象（Effects::AdjustCityYieldModifier 等 IModifierEffect 实现）布局：
// +0x00 vtable，+0x08 YieldType 数组，+0x18 条目数（+0x10 容量），
// +0x20 Amount 数组，+0x30 数量（+0x28 容量）。克隆槽数需覆盖 Apply/Remove
// 及析构槽，64 槽远大于实际接口宽度，用于按函数指针定位并替换。
// 同理加宽：效果对象 vtable 也按“足够宽”克隆，避免同类越界。
constexpr std::size_t kEffectVTableCloneSlots = 64;
constexpr std::size_t kEffectYieldTypeArrayOffset = 0x08;
constexpr std::size_t kEffectEntryCountOffset = 0x18;
constexpr std::size_t kEffectAmountArrayOffset = 0x20;
constexpr std::size_t kEffectAmountCountOffset = 0x30;

// 运行时防御上限：效果条目数与可信人口范围。条目数骤增或人口离谱通常是
// self/city 指针无效的症状，此时宁可跳过写入也不要把垃圾地址写坏。
constexpr int kMaxEffectEntries = 64;
constexpr int kMaxPlausiblePopulation = 100000;

// City::Instance 人口字段（lGetPopulation / GetYieldFromPopulation 确认）。
constexpr std::size_t kCityPopulationOffset = 0x268;

// City::Instance 城市 ID（ICity::GetID Lua 绑定读取 +0xA8 确认）。用于识别城市
// 指针被回收后复用：复用地址上的 ID 与登记时不一致即丢弃旧条目，避免误加到新城市。
constexpr std::size_t kCityIdOffset = 0xA8;

// —— 效果处理器注册表（handler registry）——
// 引擎为每个内建效果登记一个运行时 handler（哈希表的节点 +0x18）。自定义 EffectType
// 必须同样登记：否则引擎解析该效果的行为 handler 时按哈希查到无效节点，解引用得到
// 地址 0xFFFFFFFF（EXCEPTION_ACCESS_VIOLATION）。
//
// 注册表根对象每次游戏上下文构造时由 FUN_1804891b0(root) 建立：
//   root + 0x00 效果表（kind=2）、root + 0x40 集合表（kind=3）、root + 0x80 需求表。
// FUN_1806083f0(root, kind, hash, handlerObj) 按哈希设置/替换/移除 handler；
// 节点中 handler 为首次插入（0x20 字节 {next, prev, hash@+0x10, handler@+0x18}）。
constexpr int kHandlerKindEffects = 2;

// handler 对象为单指针对象：handler[0] 指向描述表。
// 描述表槽 0 = analyze、槽 1 = apply（由模板实现与运行期日志确认）。
constexpr std::size_t kHandlerAnalyzeSlot = 0;
constexpr std::size_t kHandlerApplySlot = 1;
// 表槽 4（+0x20）：引擎在关停/替换 handler 时调用 (*handler)[4](handler, 1) 作为释放例程。
// 自建对象必须覆盖此槽，否则会进入模板释放例程并解引用不存在的对象字段（退出崩溃根因）。
constexpr std::size_t kHandlerReleaseSlot = 4;
// 自建 handler 对象分配大小：清零分配，覆盖模板对象字段，避免任何槽读到堆垃圾。
constexpr std::size_t kHandlerObjectBytes = 0x40;
// 克隆宽度：足够覆盖引擎会调用的槽（实测用到 0/1 与 +0x30 等，取 16 槽留余量）。
constexpr std::size_t kHandlerTableCloneSlots = 16;

// 引擎“处理器（handler）”哈希表节点与派发：
// 节点 0x20 字节：{ next, prev, hash@+0x10, handler@+0x18 }。
// 派发 thunk（RVA 0x979290）取 handler=*(node+0x18)，再尾调用描述表槽 +0x30（槽 6）。
constexpr std::size_t kHandlerNodeHashOffset = 0x10;
constexpr std::size_t kHandlerNodeHandlerOffset = 0x18;
constexpr std::size_t kHandlerDispatchSlotOffset = 0x30;

// 模板 handler 描述表槽 1（apply）的实参布局（由 FUN_18046b390 反编译确认）：
//   args + 0x08 = Amount(int)、args + 0x0C = YieldType(int)；
//   context + 0x70 = City::Instance*。
constexpr std::size_t kEffectArgsAmountOffset = 0x08;
constexpr std::size_t kEffectArgsYieldTypeOffset = 0x0C;
constexpr std::size_t kEffectContextCityOffset = 0x70;

// Registry<T>::GetTypes() 返回的 std::vector 三指针布局。
constexpr std::size_t kVectorBeginOffset = 0x00;
constexpr std::size_t kVectorEndOffset = 0x08;
constexpr std::size_t kVectorCapacityOffset = 0x10;

extern HMODULE g_selfModule;

// 真实 GameCore 中被特征扫描解析出的内部入口。
struct GameCoreApi {
  HMODULE module = nullptr;
  void*   getEffectRegistry = nullptr; // Registry<IModifierEffectFactory>::GetTypes()
  void*   mallocTemp = nullptr;        // Platform::MallocTemp(size, file, line, a, b)
  void*   reserveVector = nullptr;     // std::vector::_Reserve(count) 成员函数
  void*   effectApply = nullptr;       // Effects::AdjustCityYieldModifier::Apply
  void*   effectRemove = nullptr;      // Effects::AdjustCityYieldModifier::Remove
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
  std::string typeName;
  bridge::EffectBehavior behavior = bridge::EffectBehavior::kInherit;
};
[[nodiscard]] std::vector<RegisteredEffect> registeredEffects();

// crash_capture.cpp：安装崩溃现场抓取（VEH）；幂等。
void installCrashCapture();
// 反安装崩溃抓取：移除 VEH，避免 DLL 卸载后 VEH 指向已卸载代码。
void uninstallCrashCapture();

// effect_handler.cpp
[[nodiscard]] bool installEffectHandlerHook();
void uninstallEffectHandlerHook();
// 移除本模组登记过的 handler 节点（在转发真实 DllDestroyGameContext 之前调用，
// 此时 root 仍有效）。请勿在其它时机调用。
void removeCustomEffectHandlers();

// vtable_clone.cpp
[[nodiscard]] void* cloneFactoryVTable(void* templateFactory);
void rememberTypeName(std::uint32_t typeHash, const char* typeName);

// custom_effect.cpp
int registerEffectBehavior(std::uint32_t typeHash, bridge::EffectBehavior behavior,
                           void* originalCreate);
void* patchEffectObjectVTable(void* effectObject, std::uint32_t typeHash);
[[nodiscard]] void* customFactoryCreateEntry();

// 每人口效果的城市累计记录：Apply/Remove 时登记与注销，人口变化时按增量补正。
// appliedPopulation 为该条目当前已应用的人口基准，(amount × appliedPopulation)
// 恒等于已写入城市的累计修正；Remove 据此精确回退（即使人口 hook 未安装）。
void rememberAppliedCityModifier(void* city, int yieldType, int amount, int appliedPopulation);
[[nodiscard]] bool forgetAppliedCityModifier(void* city, int yieldType, int amount,
                                             int& appliedTotal);
void adjustAppliedCityPopulation(void* city);
void clearAppliedCityModifiers();

// custom_effect.cpp：已注册每人口行为时确保 hook 已安装（用于新建游戏上下文时重新启用）。
bool installPopulationHookIfNeeded();

// population_hook.cpp
[[nodiscard]] bool installPopulationHook();
void uninstallPopulationHook();

// plugin_manager.cpp
void loadPlugins(bridge::Host* host);
void unloadPlugins();

// proxy.cpp
void initializeLoaderOnce();
[[nodiscard]] bool loaderInitialized();
[[nodiscard]] void* createGameContext();
void destroyGameContext(void* context);
[[nodiscard]] std::uint64_t telemetrySessionHash();

} // namespace ykkz000::loader
