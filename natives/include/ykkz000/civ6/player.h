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
//
// 证据若标注“早前符号镜像”，表示该字段的偏移取自符号丰富的旧构建、尚未在发布
// 镜像上逐条复核，仅作记录（不用于逻辑）。
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
    std::uint8_t unknown_0x0dc[0x5f4]; // 0x0DC..0x6CF: 未知
    // 0x6D0: Cities 对象指针；其 +0xD0 为城市链头（节点 next 在 +0x10）。
    //        证据：Player::Stats::GetPopulation / CalculatePlayerYieldNetRate 遍历。
    void* cities;
    std::uint8_t unknown_0x6d8[8];     // 0x6D8..0x6DF: 未知
    // 0x6E0: 外交对象指针。证据：City::Instance::CalculateYield 取
    //        GetPlayer(owner) 后经 [RAX+0x6E0] 传给外交函数。
    void* diplomacy;
    std::uint8_t unknown_0x6e8[0x18];  // 0x6E8..0x6FF: 未知
    // 0x700: 资源对象指针。证据（早前符号镜像）：
    //        Resources::GetResourceAmount(*(Resources**)(player+0x700), …)。
    void* resources;
    std::uint8_t unknown_0x708[0x18];  // 0x708..0x71F: 未知
    // 0x720: 宗教对象指针。证据（早前符号镜像）：
    //        Player::Religion::GetWorshipBuilding(*(Religion**)(player+0x720))。
    void* religion;
    std::uint8_t unknown_0x728[0x20];  // 0x728..0x747: 未知
    // 0x748: Player::Influence*（指针，非内嵌对象）——
    //        Exports::Player::GetInfluence：*(Influence**)(instance+0x748)；
    //        Influence::UpdateSuzerain：ReturnLeviedMilitary(*(Influence**)(player+0x748), …)。
    Influence* influence;
    std::uint8_t unknown_0x750[0x30];  // 0x750..0x77F: 未知
    // 0x780: 金库（Treasury）对象指针。证据（早前符号镜像）：
    //        Player::Treasury::GetTotalMaintenance(*(Treasury**)(player+0x780))。
    void* treasury;
  };

  // 引擎 GameCore::Player::Influence（城邦外交/宗主状态，每个玩家一个）。
  //
  // 构造函数（Influence::Influence）按序初始化 FAutoVariable：“m_eSuzerain”
  // (+0x408) 的值在 +0x418 且初值 -1；+0x498 为宿主 Player::Instance*（清零）。
  struct Influence {
    void** vtable;                      // 0x000: 对象 vtable
    std::uint8_t unknown_0x008[0x280];  // 0x008..0x287: 未知
    // 0x288: int32 计数表指针，按 PlayerTypes 索引。证据：Influence::UpdateSuzerain
    //        `*(int*)(*(this+0x288))[playerType] += 1`。语义待确认。
    std::int32_t* per_player_counts_0x288;
    std::uint8_t unknown_0x290[8];      // 0x290..0x297: 未知
    // 0x298: 按 PlayerTypes 的表指针，UpdateSuzerain 用于开放边境判定。语义待确认。
    std::int32_t* open_borders_0x298;
    std::uint8_t unknown_0x2a0[0x138];  // 0x2A0..0x3D7: 未知
    // 0x3D8..0x3FF: 每朝贡产出修正的 VariantMap（数据 + 边界）。证据（早前符号镜像）：
    //        Player::Influence::Get/SetYieldModifierPerTributary 使用 +0x3D8/+0x3E8/+0x3F0。
    //        语义待确认，按不透明区间记录。
    std::uint8_t per_tributary_modifier_0x3d8[0x28];
    std::uint8_t unknown_0x400[0x18];   // 0x400..0x417: 未知
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
    //       （*(*(manager+0x20) + playerType*8)）。
    Instance** players;
    std::uint8_t unknown_0x028[0x2b20]; // 0x28..0x2B47: 未知
    // 0x2B48: int32 表指针，按 PlayerTypes 索引（-1 == 无效）。证据：
    //         Influence::UpdateSuzerain（*(manager+0x2b48) + playerType*4，
    //         越界前判 0x40 < playerType）。语义待确认。
    std::int32_t* player_index_table_0x2b48;
    std::uint8_t unknown_0x2b50[8];     // 0x2B50..0x2B57: 尾部对齐
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
static_assert(offsetof(Player::Instance, cities) == 0x6d0);
static_assert(offsetof(Player::Instance, diplomacy) == 0x6e0);
static_assert(offsetof(Player::Instance, resources) == 0x700);
static_assert(offsetof(Player::Instance, religion) == 0x720);
static_assert(offsetof(Player::Instance, influence) == 0x748);
static_assert(offsetof(Player::Instance, treasury) == 0x780);

static_assert(std::is_standard_layout_v<Player::Influence>);
static_assert(offsetof(Player::Influence, per_player_counts_0x288) == 0x288);
static_assert(offsetof(Player::Influence, open_borders_0x298) == 0x298);
static_assert(offsetof(Player::Influence, per_tributary_modifier_0x3d8) == 0x3d8);
static_assert(offsetof(Player::Influence, suzerain) == 0x418);
static_assert(offsetof(Player::Influence, owner) == 0x498);

static_assert(std::is_standard_layout_v<Player::Manager>);
static_assert(offsetof(Player::Manager, players) == 0x20);
static_assert(offsetof(Player::Manager, player_index_table_0x2b48) == 0x2b48);

static_assert(std::is_standard_layout_v<Player::Stats>);
static_assert(offsetof(Player::Stats, city_list_root) == 0xe18);

static_assert(std::is_standard_layout_v<Player::CityID>);
static_assert(sizeof(Player::CityID) == 8);
static_assert(offsetof(Player::CityID, kind) == 0x02);
static_assert(offsetof(Player::CityID, id) == 0x04);

} // namespace ykkz000::civ6
