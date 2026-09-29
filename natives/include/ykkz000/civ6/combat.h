#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <ykkz000/civ6/district.h>
#include <ykkz000/civ6/unit.h>

namespace ykkz000::civ6 {

// 引擎 GameEffects 命名空间。
struct GameEffects {
  // 引擎 GameEffects::ProposedCombat：战斗力修正落账结构，即写入点函数
  // AdjustPlayerStrengthModifier(PlayerTypes, int) 的 this。
  //
  // 落账规则（AdjustPlayerStrengthModifier 全反编译）：
  //   - +0x00 单位非空且 unit+0x128 == playerType ⇒ +0x2C += amount；
  //   - 否则 +0x08 区域非空且 District::Instance::GetOwner() == playerType
  //     ⇒ +0x2C += amount；
  //   - 均不匹配 ⇒ +0x30 += amount。
  struct ProposedCombat {
    Unit::Instance* unit;              // 0x00: 目标单位（非空即按单位定向落账）
    District::Instance* district;      // 0x08: 目标区域（非空则按区域所有者落账）
    std::uint8_t unknown_0x10[0x1c];   // 0x10..0x2B: 未知
    std::int32_t directed_total;       // 0x2C: 单位/区域定向累计
    std::int32_t general_total;        // 0x30: 通用累计（未匹配目标单位/区域）
  };
};

static_assert(std::is_standard_layout_v<GameEffects::ProposedCombat>);
static_assert(offsetof(GameEffects::ProposedCombat, unit) == 0x00);
static_assert(offsetof(GameEffects::ProposedCombat, district) == 0x08);
static_assert(offsetof(GameEffects::ProposedCombat, directed_total) == 0x2c);
static_assert(offsetof(GameEffects::ProposedCombat, general_total) == 0x30);

} // namespace ykkz000::civ6
