#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <ykkz000/civ6/player.h>

namespace ykkz000::civ6 {

// 引擎 GameCore::Unit::Instance。
struct Unit {
  struct Instance {
    void** vtable;                      // 0x000: 单位对象 vtable
    std::uint8_t unknown_0x008[0x120];  // 0x008..0x127: 未知
    // 0x128: PlayerTypes（所有者）—— GameEffects::ProposedCombat::
    //        AdjustPlayerStrengthModifier：*(PlayerTypes*)(unit+0x128) == playerType。
    PlayerTypes owner;
  };
};

static_assert(std::is_standard_layout_v<Unit::Instance>);
static_assert(offsetof(Unit::Instance, owner) == 0x128);

} // namespace ykkz000::civ6
