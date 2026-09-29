#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <ykkz000/civ6/city.h>
#include <ykkz000/civ6/player.h>
#include <ykkz000/civ6/unit.h>

namespace ykkz000::civ6 {

// Effects::AdjustCityYieldModifier 的运行时对象（城市产出修正效果）。
//
// 与玩家战斗力模板不同，它把参数存为两个并列数组（YieldType[] / Amount[]），
// 并把“条目数”与“数量”分别记录在 +0x18 / +0x30。clone 的 vtable 会按函数指针
// 定位并替换 Apply/Remove 槽（见 loader 的 effect_mechanism.cpp）。
struct CityYieldModifierEffect {
  void** vtable;                // 0x00: 效果对象 vtable
  int* yields;                  // 0x08: YieldType 数组
  std::uint8_t unknown_0x10[8]; // 0x10: 未知（注释记载为数组容量）
  std::int32_t entry_count;     // 0x18: 条目数
  std::uint8_t unknown_0x1c[4]; // 0x1C: 未知
  int* amounts;                 // 0x20: Amount 数组
  std::uint8_t unknown_0x28[8]; // 0x28: 未知（注释记载为数组容量）
  std::int32_t amount_count;    // 0x30: 数量
  std::uint8_t unknown_0x34[4]; // 0x34: 未知
};

static_assert(std::is_standard_layout_v<CityYieldModifierEffect>);
static_assert(offsetof(CityYieldModifierEffect, yields) == 0x08);
static_assert(offsetof(CityYieldModifierEffect, entry_count) == 0x18);
static_assert(offsetof(CityYieldModifierEffect, amounts) == 0x20);
static_assert(offsetof(CityYieldModifierEffect, amount_count) == 0x30);

// Effects::AdjustPlayerStrengthModifier 的运行时对象（玩家战斗力修正效果）。
//
// 该效果不像城市产出模板那样使用数组，而是把参数存为标量：+0x40 Amount（由
// FUN_1808d5cf0 解析 "Amount" 写入）。Apply（FUN_1808D9110）的已核验语义：
//   - +0x44 != 100 时按 powf 指数叠加（叠加计数由 FUN_180951d20 维护）；
//   - +0x48 为上限（cap）；
//   - +0x4C 为作用域（域，-1 表示关闭）；
//   - +0x50 为千分比标量（应用时 ×0x6400）；
//   - +0x54 按回合/时代缩放百分比（FUN_18063d0e0/FUN_18063d160）；
//   - +0x58 按玩家数缩放（读 GameManager+0x10D0/+0x10D8）；
//   - 结果累加进 +0x5C（已应用总量），玩家身份取 ownerObj+0xD8。
// Remove（FUN_1808DA240）按 +0x5C 精确回退。
// 注：字段名沿用偏移命名，个别名称的精确语义仍以“未验证”为准。
struct AdjustPlayerStrengthModifier {
  void** vtable;                               // 0x00: 效果对象 vtable
  std::uint8_t unknown_0x08[0x38];             // 0x08..0x3F: 未知
  std::int32_t amount;                         // 0x40: Amount（解析自 "Amount"）
  std::int32_t stack_percent_0x44;             // 0x44: 指数式 StackPercent（!=100 走 powf）
  std::int32_t cap_0x48;                       // 0x48: 上限/逆向缩放
  std::int32_t domain_0x4c;                    // 0x4C: 作用域域（-1 关闭）
  std::int32_t scalar_0x50;                    // 0x50: 千分比标量（应用时 ×0x6400）
  std::int32_t advanced_start_multiplier_0x54; // 0x54: 按回合/时代缩放百分比
  std::int32_t scale_by_player_count_0x58;     // 0x58: 按玩家数缩放
  std::int32_t applied_total;                  // 0x5C: 已应用总量（预览与精确回退来源）
};

static_assert(std::is_standard_layout_v<AdjustPlayerStrengthModifier>);
static_assert(offsetof(AdjustPlayerStrengthModifier, amount) == 0x40);
static_assert(offsetof(AdjustPlayerStrengthModifier, stack_percent_0x44) == 0x44);
static_assert(offsetof(AdjustPlayerStrengthModifier, cap_0x48) == 0x48);
static_assert(offsetof(AdjustPlayerStrengthModifier, domain_0x4c) == 0x4c);
static_assert(offsetof(AdjustPlayerStrengthModifier, scalar_0x50) == 0x50);
static_assert(offsetof(AdjustPlayerStrengthModifier, advanced_start_multiplier_0x54) == 0x54);
static_assert(offsetof(AdjustPlayerStrengthModifier, scale_by_player_count_0x58) == 0x58);
static_assert(offsetof(AdjustPlayerStrengthModifier, applied_total) == 0x5c);
static_assert(sizeof(AdjustPlayerStrengthModifier) == 0x60);

// 模板 handler 的 apply 槽（槽 1）实参（由 FUN_18046b390 反编译确认）。
struct EffectArgs {
  std::uint8_t unknown_0x00[8]; // 0x00: 未知
  std::int32_t amount;          // 0x08: Amount
  std::int32_t yield_type;      // 0x0C: YieldType
};

static_assert(std::is_standard_layout_v<EffectArgs>);
static_assert(offsetof(EffectArgs, amount) == 0x08);
static_assert(offsetof(EffectArgs, yield_type) == 0x0c);
static_assert(sizeof(EffectArgs) == 0x10);

// 效果应用上下文。+0x70 的 City 是城市产出模板实际使用的字段（FUN_180464310）；
// +0x80/+0x90 的 Player/Unit 仅由 FUN_180465AD0 的旧猜测记载，未被 loader 使用。
struct EffectContext {
  std::uint8_t unknown_0x00[0x70]; // 0x00..0x6F: 未知
  City::Instance* city;            // 0x70: 城市实例
  std::uint8_t unknown_0x78[8];    // 0x78: 未知
  Player::Instance* player;        // 0x80: 玩家（未验证）
  std::uint8_t unknown_0x88[8];    // 0x88: 未知
  Unit::Instance* unit;            // 0x90: 单位（未验证）
};

static_assert(std::is_standard_layout_v<EffectContext>);
static_assert(offsetof(EffectContext, city) == 0x70);
static_assert(offsetof(EffectContext, player) == 0x80);
static_assert(offsetof(EffectContext, unit) == 0x90);

} // namespace ykkz000::civ6
