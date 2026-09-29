#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <ykkz000/civ6/common.h>

namespace ykkz000::civ6 {

// 每修饰器应用的运行时上下文（DynamicModifier 实例）。
//
// 由 FUN_18092A4F0 构造，驱动入口 FUN_180944D60 的第一实参即该实例：
//   +0x148（+0x150 为控制块）= owner 值（构造参数 param_3）；
//   +0x1B8（+0x1C0 为控制块）= 效果对象（构造参数 param_6，即打过 vtable 补丁的对象）。
// 以 ctx+0x1B8 == self 校验归属后，ctx+0x148 即本效果对象的宿主（任意携带玩家 ID 的
// 引擎对象，未必是 Player::Instance*，先用 Player::Instance::player_type 偏移解析）。
//
// 其余字段仅用于记录已知偏移：类型标注“未验证”者为暂定类型，不参与 loader 行为。
struct DynamicModifierContext {
  void** vtable;                        // 0x00: 上下文 vtable
  std::uint8_t unknown_0x08[0xe0];      // 0x08..0xE7: 未知
  std::uint32_t effect_type_0xe8;       // 0xE8: effect_type（类型未验证）
  std::uint8_t unknown_0xec[0x14];      // 0xEC..0xFF: 未知
  std::uint8_t active_flag_0x100;       // 0x100: active 标志（类型未验证）
  std::uint8_t unknown_0x101[0x2f];     // 0x101..0x12F: 未知
  std::uint8_t disabled_flag_0x130;     // 0x130: disabled 标志（类型未验证）
  std::uint8_t unknown_0x131[0x7];      // 0x131..0x137: 未知
  SharedPtrView<void> owner_property;   // 0x138: owner_property（疑似 shared_ptr）
  void* owner;                          // 0x148: owner 值（宿主对象）
  void* owner_control_0x150;            // 0x150: owner 的控制块
  SharedPtrView<void> definition_property; // 0x158: definition_property
  void* collection_provider_0x168;      // 0x168: collection provider（未验证）
  std::uint8_t unknown_0x170[0x8];      // 0x170..0x177: 未知
  SharedPtrView<void> subjects_property; // 0x178: subjects_property
  std::uint8_t unknown_0x188[0x20];     // 0x188..0x1A7: 未知
  void* subject_provider_0x1a8;         // 0x1A8: subject provider（未验证）
  std::uint8_t unknown_0x1b0[0x8];      // 0x1B0..0x1B7: 未知
  void* effect_object;                  // 0x1B8: 效果对象（打过 vtable 补丁的实例）
  void* effect_object_control_0x1c0;    // 0x1C0: 效果对象的控制块
  std::uint8_t unknown_0x1c8[0x10];     // 0x1C8..0x1D7: 未知
  std::int32_t reentry_depth_0x1d8;     // 0x1D8: 重入深度（类型未验证）
  std::uint8_t unknown_0x1dc[0x24];     // 0x1DC..0x1FF: 未知
  void* subject_collection_0x200;       // 0x200: subject collection（未验证）
};

static_assert(std::is_standard_layout_v<DynamicModifierContext>);
static_assert(offsetof(DynamicModifierContext, effect_type_0xe8) == 0xe8);
static_assert(offsetof(DynamicModifierContext, active_flag_0x100) == 0x100);
static_assert(offsetof(DynamicModifierContext, disabled_flag_0x130) == 0x130);
static_assert(offsetof(DynamicModifierContext, owner_property) == 0x138);
static_assert(offsetof(DynamicModifierContext, owner) == 0x148);
static_assert(offsetof(DynamicModifierContext, owner_control_0x150) == 0x150);
static_assert(offsetof(DynamicModifierContext, definition_property) == 0x158);
static_assert(offsetof(DynamicModifierContext, collection_provider_0x168) == 0x168);
static_assert(offsetof(DynamicModifierContext, subjects_property) == 0x178);
static_assert(offsetof(DynamicModifierContext, subject_provider_0x1a8) == 0x1a8);
static_assert(offsetof(DynamicModifierContext, effect_object) == 0x1b8);
static_assert(offsetof(DynamicModifierContext, effect_object_control_0x1c0) == 0x1c0);
static_assert(offsetof(DynamicModifierContext, reentry_depth_0x1d8) == 0x1d8);
static_assert(offsetof(DynamicModifierContext, subject_collection_0x200) == 0x200);

// 效果对象 Apply 槽的实参 a1 为 subject 包装（FUN_180944D60 的调用约定）：
// a1+0x00 即 subject 对象本体。其余字段仅按偏移记录（FUN_180949820 /
// FUN_180943CD0 佐证），类型未验证。
struct EffectSubjectWrapper {
  void* object;                         // 0x00: subject 对象本体
  std::uint8_t unknown_0x08[0x8];       // 0x08..0x0F: 未知
  std::uint16_t flags_0x10;             // 0x10: 标志位（类型未验证）
  std::uint8_t unknown_0x12[0xe];       // 0x12..0x1F: 未知
  SharedPtrView<void> collection_0x20;  // 0x20: 集合（共享指针）
  std::uint8_t unknown_0x30[0x10];      // 0x30..0x3F: 未知
  std::uint8_t flag_0x40;               // 0x40: 标志（类型未验证）
  std::uint8_t flag_0x41;               // 0x41: 标志（类型未验证）
};

static_assert(std::is_standard_layout_v<EffectSubjectWrapper>);
static_assert(offsetof(EffectSubjectWrapper, object) == 0x00);
static_assert(offsetof(EffectSubjectWrapper, flags_0x10) == 0x10);
static_assert(offsetof(EffectSubjectWrapper, collection_0x20) == 0x20);
static_assert(offsetof(EffectSubjectWrapper, flag_0x40) == 0x40);
static_assert(offsetof(EffectSubjectWrapper, flag_0x41) == 0x41);

} // namespace ykkz000::civ6
