#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <ykkz000/civ6/common.h>
#include <ykkz000/civ6/player.h>

namespace ykkz000::civ6 {

// GameManager 对象（FUN_180044d60() 返回）。
//
// +0x50 起是玩家指针向量，按索引取 Player::Instance*。游戏以
// Context::Globals::EditPlayerManager() 返回同一对象（UpdateSuzerain 亦读
// +0x50/+0x58 的玩家向量）。玩家向量下标不保证等于玩家类型，需要精确匹配时逐项
// 读 Player::Instance::player_type 比对。
struct GameManager {
  void** vtable;                                // 0x00: 对象 vtable
  std::uint8_t unknown_0x08[0x48];              // 0x08..0x4F: 未知
  VectorView<Player::Instance*> players;        // 0x50: 玩家指针向量（begin/end/capacity）
  std::uint8_t unknown_0x68[0x1068];            // 0x68..0x10CF: 未知
  VectorView<void*> unknown_vector_0x10d0;      // 0x10D0: 另一向量（元素 8 字节，未验证）
};

static_assert(std::is_standard_layout_v<GameManager>);
static_assert(offsetof(GameManager, players) == 0x50);
static_assert(offsetof(GameManager, players) +
                  offsetof(VectorView<Player::Instance*>, begin) == 0x50);
static_assert(offsetof(GameManager, players) +
                  offsetof(VectorView<Player::Instance*>, end) == 0x58);
static_assert(offsetof(GameManager, unknown_vector_0x10d0) == 0x10d0);

} // namespace ykkz000::civ6
