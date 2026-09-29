#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <ykkz000/civ6/common.h>

namespace ykkz000::civ6 {

// Registry<IModifierEffectFactory> 的条目对象（自定义效果注册时由
// Platform::MallocTemp 分配，见 registry.cpp）。
struct ModifierEffectFactory {
  void** vtable;                // 0x00: 工厂 vtable（槽位见 common.h 的 kFactory*Slot）
  std::uint32_t type_hash;      // 0x08: EffectType 哈希；等价于 GetTypeId() 的返回值
  std::uint8_t unknown_0x0c[4]; // 0x0C: 未知（对齐填充）
};

static_assert(std::is_standard_layout_v<ModifierEffectFactory>);
static_assert(offsetof(ModifierEffectFactory, type_hash) == 0x08);
static_assert(sizeof(ModifierEffectFactory) == 0x10);

} // namespace ykkz000::civ6
