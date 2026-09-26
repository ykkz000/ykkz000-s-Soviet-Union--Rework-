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
};

[[nodiscard]] std::wstring moduleDirectory();
[[nodiscard]] bool ensureGameCoreLoaded();
[[nodiscard]] const GameCoreApi& gameCore();

[[nodiscard]] std::uint32_t makeHash(const char* text);
void logMessage(int level, const char* message);

// registry.cpp
[[nodiscard]] int registerEffectType(const bridge::EffectDesc* desc);

// vtable_clone.cpp
[[nodiscard]] void* cloneFactoryVTable(void* templateFactory);
void rememberTypeName(std::uint32_t typeHash, const char* typeName);

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
