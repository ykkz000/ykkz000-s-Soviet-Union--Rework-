#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <ykkz000/civ6/player.h>

namespace ykkz000::civ6 {

// 引擎 GameCore::Unit::Instance。
//
// 单位身份：+0xB0 为单位 ID（发布镜像 FUN_18005c4a0 以 `*(int*)(unit+0xB0)` 打印
// “Player %i Unit %i …”；FUN_1803a48d0 以 `*(int*)(unit+0xB0)` 与该单位所在列表里
// 的 ID 逐项比对）。同一玩家的 UnitManager（Player+0x6C8）用
// `FUN_1802c4360(manager, unitId)` 以该 ID 反查单位（低 16 位为槽位），故
// (owner, unit_id) 在玩家内可作稳定键；槽位会被回收，读取侧仍需回读校验。
struct Unit {
  struct Instance {
    void** vtable;                      // 0x000: 单位对象 vtable
    std::uint8_t unknown_0x008[0xa8];   // 0x008..0x0AF: 未知
    // 0x0B0: 单位 ID（uint32；Player::UnitManager 的键）。
    std::int32_t unit_id;
    std::uint8_t unknown_0x0b4[0x74];   // 0x0B4..0x127: 未知
    // 0x128: PlayerTypes（所有者）—— GameEffects::ProposedCombat::
    //        AdjustPlayerStrengthModifier：*(PlayerTypes*)(unit+0x128) == playerType。
    PlayerTypes owner;
  };
};

static_assert(std::is_standard_layout_v<Unit::Instance>);
static_assert(offsetof(Unit::Instance, unit_id) == 0xb0);
static_assert(offsetof(Unit::Instance, owner) == 0x128);

} // namespace ykkz000::civ6
