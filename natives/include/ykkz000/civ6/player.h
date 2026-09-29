#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

// GameCore 引擎运行时内存布局（GameCore_XP2_FinalRelease.dll，逆向确认）。
//
// 本目录只描述布局：类型仅含 POD 数据成员，不含虚函数、基类与非平凡成员。
// 结构体命名镜像引擎的嵌套类型（Player::Instance / Player::Influence …），
// 便于与带符号的反编译一一对应。每个已知字段以 static_assert 锁定偏移，并在
// 注释中给出证据函数名；因镜像不同，这里只搬“偏移/语义”，绝不搬函数 RVA。
namespace ykkz000::civ6 {

// 引擎枚举 GameCore::PlayerTypes（4 字节）。本模组只把它当“玩家类型/ID”的
// 类型标注，不依赖具体枚举值。
enum class PlayerTypes : std::int32_t {};

// 引擎以 -1（0xFFFFFFFF）表示“无/未设置”玩家类型：
//   - Player::Influence 的 m_eSuzerain(+0x418) 初值即为 -1；
//   - 玩家向量下标与玩家类型不保证一致（城邦/蛮族会跳号）。
inline constexpr PlayerTypes kInvalidPlayerType = static_cast<PlayerTypes>(-1);

// 取 PlayerTypes 的底层整数，供 kMaxPlausiblePlayerIndex 之类的范围校验比较。
[[nodiscard]] inline constexpr std::int32_t PlayerTypeIndex(PlayerTypes type) noexcept {
  return static_cast<std::int32_t>(type);
}

// 引擎 GameCore::Player 命名空间（以嵌套结构镜像其类型）。
struct Player {
  struct Instance;
  struct Influence;

  // 引擎 GameCore::Player::Instance。
  struct Instance {
    void** vtable;                     // 0x000: 对象 vtable
    std::uint8_t unknown_0x008[0xd0];  // 0x008..0x0D7: 未知
    // 0x0D8: PlayerTypes —— AI::Economic::GetOwner（*(this+0x98)+0xD8）、
    //        Player::Instance::GetLevel / IsMinor（*(this+0xD8)）、
    //        Influence::ChangeTokensReceivedModifier（*(this+0x498)+0xD8）。
    PlayerTypes player_type;
    std::uint8_t unknown_0x0dc[0x66c]; // 0x0DC..0x747: 未知
    // 0x748: Player::Influence*（指针，非内嵌对象）——
    //        Exports::Player::GetInfluence：*(Influence**)(instance+0x748)；
    //        Influence::UpdateSuzerain：ReturnLeviedMilitary(*(Influence**)(player+0x748), …)。
    Influence* influence;
  };

  // 引擎 GameCore::Player::Influence（城邦外交/宗主状态，每个玩家一个）。
  //
  // 构造函数（Influence::Influence）按序初始化 FAutoVariable：“m_eSuzerain”
  // (+0x408) 的值在 +0x418 且初值 -1；+0x498 为宿主 Player::Instance*（清零）。
  struct Influence {
    void** vtable;                      // 0x000: 对象 vtable
    std::uint8_t unknown_0x008[0x410];  // 0x008..0x417: 未知
    // 0x418: PlayerTypes —— 该 Influence 所属城邦的宗主（m_eSuzerain）。
    //        Influence::GetSuzerain 仅 return *(PlayerTypes*)(this+0x418)；
    //        UpdateSuzerain 在宗主变更时写回该值。
    PlayerTypes suzerain;
    std::uint8_t unknown_0x41c[0x7c];   // 0x41C..0x497: 未知
    // 0x498: Player::Instance* —— 该 Influence 的宿主玩家。
    //        Influence::ChangeTokensReceivedModifier 把它作信号参数派发，
    //        并以 *(this+0x498)+0xD8 取宿主玩家类型。
    Instance* owner;
  };

  // 引擎 GameCore::Player::Manager（Player::Manager::EditInstance() 返回）。
  struct Manager {
    void** vtable;                      // 0x00: 对象 vtable
    std::uint8_t unknown_0x008[0x18];   // 0x08..0x1F: 未知
    // 0x20: Player::Instance*[]，按 PlayerTypes 索引、步长 8；有效下标 < 0x40。
    //       证据：Influence::ChangeTokensReceivedModifier
    //       （*(*(manager+0x20) + playerType*8)）、UpdateSuzerain
    //       （*(manager+0x2b48) + playerType*4，越界前判 0x40 < playerType）。
    Instance** players;
  };

  // 引擎 GameCore::Player::Stats。
  struct Stats {
    void** vtable;                      // 0x000: 对象 vtable
    std::uint8_t unknown_0x008[0xe10];  // 0x008..0xE17: 未知
    // 0xE18: 城市链根的中转指针。Player::Stats::GetPopulation 沿
    //        [this+0xE18] -> +0x6D0（Cities）-> +0xD0（链头）遍历城市链
    //        （节点 next 在 +0x10），并累加 city+0x268（人口）。
    void* city_list_root;
  };

  // 引擎 Player::CityID（8 字节）：{int16 玩家; int16 组件标签(恒 2); int32 序列号}。
  // 证据：Lua::Utility::GetCityID / Exports::District::GetCityID 的写出序列
  //       （*(short*)out = owner; *(short*)(out+2) = 2; *(int*)(out+4) = serial）。
  struct CityID {
    std::int16_t player_type;  // 0x00: 所属玩家（短整型）
    std::int16_t kind;         // 0x02: 组件类型标签（城市为 2）
    std::int32_t id;           // 0x04: 城市序列号（即 City::Instance+0xA8）
  };
};

static_assert(std::is_standard_layout_v<Player::Instance>);
static_assert(offsetof(Player::Instance, player_type) == 0xd8);
static_assert(offsetof(Player::Instance, influence) == 0x748);

static_assert(std::is_standard_layout_v<Player::Influence>);
static_assert(offsetof(Player::Influence, suzerain) == 0x418);
static_assert(offsetof(Player::Influence, owner) == 0x498);

static_assert(std::is_standard_layout_v<Player::Manager>);
static_assert(offsetof(Player::Manager, players) == 0x20);

static_assert(std::is_standard_layout_v<Player::Stats>);
static_assert(offsetof(Player::Stats, city_list_root) == 0xe18);

static_assert(std::is_standard_layout_v<Player::CityID>);
static_assert(sizeof(Player::CityID) == 8);
static_assert(offsetof(Player::CityID, kind) == 0x02);
static_assert(offsetof(Player::CityID, id) == 0x04);

} // namespace ykkz000::civ6
