#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include <ykkz000/bridge.h>

namespace ykkz000::loader {

// —— 引擎侧布局（由 GameCore_XP2_FinalRelease.dll 逆向确认），单一事实来源 ——
// IModifierEffectFactory 工厂对象：+0x00 vtable，+0x08 uint32 类型哈希。
constexpr std::size_t kFactoryObjectSize = 0x10;
constexpr std::size_t kFactoryHashOffset = 0x08;
constexpr std::size_t kFactoryVTableSlots = 6;
constexpr std::size_t kFactoryTypeIdSlot = 1;
constexpr std::size_t kFactoryTypeNameSlot = 2;
constexpr std::size_t kFactoryCreateSlot = 5;

// 效果对象（Effects::AdjustCityYieldModifier 等 IModifierEffect 实现）布局：
// +0x00 vtable，+0x08 YieldType 数组，+0x18 条目数（+0x10 容量），
// +0x20 Amount 数组，+0x30 数量（+0x28 容量）。克隆槽数需覆盖 Apply/Remove
// 及析构槽，32 槽远大于实际接口宽度，用于按函数指针定位并替换。
constexpr std::size_t kEffectVTableCloneSlots = 32;
constexpr std::size_t kEffectYieldTypeArrayOffset = 0x08;
constexpr std::size_t kEffectEntryCountOffset = 0x18;
constexpr std::size_t kEffectAmountArrayOffset = 0x20;
constexpr std::size_t kEffectAmountCountOffset = 0x30;

// City::Instance 人口字段（lGetPopulation / GetYieldFromPopulation 确认）。
constexpr std::size_t kCityPopulationOffset = 0x268;

// City::Instance 城市 ID（ICity::GetID Lua 绑定读取 +0xA8 确认）。用于识别城市
// 指针被回收后复用：复用地址上的 ID 与登记时不一致即丢弃旧条目，避免误加到新城市。
constexpr std::size_t kCityIdOffset = 0xA8;

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
};

[[nodiscard]] std::wstring moduleDirectory();
[[nodiscard]] bool ensureGameCoreLoaded();
[[nodiscard]] const GameCoreApi& gameCore();

[[nodiscard]] std::uint32_t makeHash(const char* text);
void logMessage(int level, const char* message);
void logMessage(int level, const std::wstring& message);

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
