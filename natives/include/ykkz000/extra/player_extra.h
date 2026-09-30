#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <shared_mutex>
#include <type_traits>
#include <unordered_map>

#include <ykkz000/civ6/common.h> // civ6::kMaxYields
#include <ykkz000/civ6/player.h> // PlayerTypes / PlayerTypeIndex

// 目录语义：
//   civ6/  = 引擎对象布局的镜像（只描述引擎真实存在的字段/类型，不引入本模组数据）。
//   extra/ = 本模组叠加的数据与侧表（不属于引擎，可被 loader 与各插件共享）。
//
// 唯一的顶层扩展数据侧表：`PlayerExtras()`，键 = player_id（Player::Instance +0xD8 的
// PlayerTypes）。每名玩家条目内置两张子表：
//   city_extras：键 = city_id（City::Instance +0xA8，玩家内唯一）；
//   unit_extras：键 = unit_id（Unit::Instance +0xB0，玩家内唯一）。
//
// 顶层键取玩家 ⇒ 重复文明/领袖的两名玩家天然隔离；清理也以玩家为单位（ClearPlayer
// 一次清掉该玩家的全部城市/单位条目），无需按 id 扫陈旧条目。
//
// 注意：PlayerExtra 含有 unordered_map，**不再是可平凡拷贝的类型**。只读热路径一律
// 走单条目查询（FindCity 快照 / FindUnitStrength 标量），**禁止**拷贝整个 PlayerExtra
// 或整张子表——那会把“每玩家的全部城市与单位”都复制一遍。
namespace ykkz000::extra {

// 每座城市的扩展数据（键 = city_id，位于 PlayerExtra::city_extras）。
//
// percent[y]：该城市在该产出上的“每市民百分比”，以 FixedPoint<16> 存储，
//   0x10000 == +1.0%/市民（即 1.0 表示“每市民 +1%”）。
//
// 读取路径按 (percent[y] * population) >> 8 折算进引擎的修正单位：
//   引擎的修正子对象以 FixedPoint<8> 累计“百分点”（1.0 == +1%），
//   最终产出 = base * (1 + modifier / 25600)（25600 == +100%）。
//   例：Amount=5（每市民 +5%）、population=10：
//     percent = 5 * 0x10000 = 327680
//     delta   = (327680 * 10) >> 8 = 12800 == +50%（12800 / 25600）
//   故 FixedPoint<16>“百分点”在本表示下与 (x * pop) >> 8 恰好无精度损失。
//
// 聚合语义：Apply 时 +=，Remove 时 -=，与调用顺序/实例个数无关（同参数重复实例
// 不再有歧义）；向量全零即擦除，避免残留。
//
// per_suzerain_percent[y]：该城市在该产出上的“每宗主城邦百分比”，与 percent[y] 同
//   单位（FixedPoint<16>，0x10000 == +1.0%/宗主）。宗主数属运行期变量，故不在写入时
//   乘算，而是在读取路径按 (per_suzerain_percent[y] × 当前宗主数) >> 8 折算进引擎修正
//   单位，使宗主数变化被下一次读取自然跟随。
//
// owner_id 仅作冗余校验：读取时回读 City+0xD8 比对，防指针回收/易主错配。
struct CityExtra {
  std::int32_t city_id = -1;    // City::Instance +0xA8
  std::int32_t owner_id = -1;   // City::Instance +0xD8（PlayerTypes）
  std::int32_t yield_count = 0; // 有效长度（≤ civ6::kMaxYields），两张数组共用
  std::array<std::int32_t, civ6::kMaxYields> percent{};
  std::array<std::int32_t, civ6::kMaxYields> per_suzerain_percent{};
};

static_assert(std::is_standard_layout_v<CityExtra>);
static_assert(std::is_trivially_copyable_v<CityExtra>);

// 每个单位的扩展数据（键 = unit_id，位于 PlayerExtra::unit_extras）。
//
// strength_per_suzerain：该单位适用的各效果实例“每宗主城邦”平值战斗力之和，
//   单位同 AdjustPlayerStrengthModifier::amount（+0x40）。写入点钩子按
//   strength_per_suzerain × 当前宗主城邦数 落账。
//
// instances：以效果对象（instance = self）为键的 upsert 映射，保证幂等——同一实例
//   被重复 Apply 只覆盖、不重复累加（引擎重放 Apply/Remove 时不会膨胀）；
//   strength_per_suzerain 恒等于各值之和。
//
// 预留：后续 per-unit 量在此追加（保持只追加、不改已有字段偏移）。
struct UnitExtra {
  std::int32_t unit_id = -1;             // Unit::Instance +0xB0
  std::int32_t strength_per_suzerain = 0; // Σ instances
  std::unordered_map<void*, std::int32_t> instances; // key = 效果对象 self
};

// 每名玩家的扩展数据（键 = player_id），内置城市/单位两张子表。
struct PlayerExtra {
  std::int32_t player_id = -1; // Player::Instance +0xD8（PlayerTypes）
  std::unordered_map<std::int32_t, CityExtra> city_extras; // key = city_id
  std::unordered_map<std::int32_t, UnitExtra> unit_extras; // key = unit_id
};

struct PlayerKey {
  std::int32_t player_id = -1;
};

struct PlayerKeyHash {
  std::size_t operator()(const PlayerKey& key) const noexcept {
    // 与 CityKeyHash 同风格：先混合再展开，避免低位聚集。
    std::uint64_t value = static_cast<std::uint32_t>(key.player_id);
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    return static_cast<std::size_t>(value);
  }
};

struct PlayerKeyEqual {
  bool operator()(const PlayerKey& a, const PlayerKey& b) const noexcept {
    return a.player_id == b.player_id;
  }
};

// 线程安全侧表：单一 shared_mutex 覆盖顶层与两张子表（写路径独占、读路径共享）。
class PlayerExtraTable {
 public:
  // 写路径：在独占锁内取/建并编辑某玩家的顶层条目。回调形式（而非返回引用）保证
  // “读改写”全程持锁，避免与共享锁读路径竞争。
  template <typename Fn>
  void EditPlayer(const PlayerKey& key, Fn&& fn) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    fn(PlayerFor(key.player_id));
  }

  // 写路径：编辑某玩家的单个城市条目（不存在则按 city_id/owner_id 建立）。
  template <typename Fn>
  void EditCity(std::int32_t player_id, std::int32_t city_id,
                std::int32_t owner_id, Fn&& fn) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    PlayerExtra& player = PlayerFor(player_id);
    CityExtra& city = player.city_extras[city_id];
    if (city.city_id != city_id || city.owner_id != owner_id) {
      city = CityExtra{};
      city.city_id = city_id;
      city.owner_id = owner_id;
    }
    fn(city);
  }

  // 写路径：编辑某玩家的单个单位条目（不存在则建立）。
  template <typename Fn>
  void EditUnit(std::int32_t player_id, std::int32_t unit_id, Fn&& fn) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    PlayerExtra& player = PlayerFor(player_id);
    UnitExtra& unit = player.unit_extras[unit_id];
    if (unit.unit_id != unit_id) {
      unit = UnitExtra{};
      unit.unit_id = unit_id;
    }
    fn(unit);
  }

  // 只读查询（热路径）：城市单条目快照（CityExtra 可平凡拷贝，无堆分配）。
  bool FindCity(std::int32_t player_id, std::int32_t city_id,
                CityExtra& out) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto player_it = table_.find(PlayerKey{player_id});
    if (player_it == table_.end()) {
      return false;
    }
    const auto city_it = player_it->second.city_extras.find(city_id);
    if (city_it == player_it->second.city_extras.end()) {
      return false;
    }
    out = city_it->second;
    return true;
  }

  // 只读查询（热路径）：仅取单位的聚合标量，**不拷贝 instances 映射**（避免每次
  // 读取都堆分配）。无条目与聚合为 0 一律返回 0，调用方据此回退到模板 Amount。
  std::int32_t FindUnitStrength(std::int32_t player_id,
                                std::int32_t unit_id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto player_it = table_.find(PlayerKey{player_id});
    if (player_it == table_.end()) {
      return 0;
    }
    const auto unit_it = player_it->second.unit_extras.find(unit_id);
    if (unit_it == player_it->second.unit_extras.end()) {
      return 0;
    }
    return unit_it->second.strength_per_suzerain;
  }

  // 两张数组全零后擦除该城市条目；子表全空时连玩家条目一并擦除，避免残留空壳。
  void EraseIfEmptyCity(std::int32_t player_id, std::int32_t city_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto player_it = table_.find(PlayerKey{player_id});
    if (player_it == table_.end()) {
      return;
    }
    PlayerExtra& player = player_it->second;
    const auto city_it = player.city_extras.find(city_id);
    if (city_it == player.city_extras.end()) {
      return;
    }
    for (const std::int32_t value : city_it->second.percent) {
      if (value != 0) {
        return;
      }
    }
    for (const std::int32_t value : city_it->second.per_suzerain_percent) {
      if (value != 0) {
        return;
      }
    }
    player.city_extras.erase(city_it);
    ErasePlayerIfBothEmpty(player_it);
  }

  // 无实例且聚合归零后擦除该单位条目；子表全空时连玩家条目一并擦除。
  void EraseIfEmptyUnit(std::int32_t player_id, std::int32_t unit_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto player_it = table_.find(PlayerKey{player_id});
    if (player_it == table_.end()) {
      return;
    }
    PlayerExtra& player = player_it->second;
    const auto unit_it = player.unit_extras.find(unit_id);
    if (unit_it == player.unit_extras.end()) {
      return;
    }
    if (!unit_it->second.instances.empty() ||
        unit_it->second.strength_per_suzerain != 0) {
      return;
    }
    player.unit_extras.erase(unit_it);
    ErasePlayerIfBothEmpty(player_it);
  }

  // 玩家消亡/被淘汰：清掉该玩家的全部城市/单位条目。
  void ClearPlayer(const PlayerKey& key) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    table_.erase(key);
  }

  void Clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    table_.clear();
  }

  // 诊断用：一次性统计表规模（仅在告警/自检路径调用，热路径不用）。
  struct Stats {
    std::size_t players = 0;
    std::size_t cities = 0;
    std::size_t units = 0;
  };

  Stats CountStats() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    Stats stats;
    stats.players = table_.size();
    for (const auto& entry : table_) {
      stats.cities += entry.second.city_extras.size();
      stats.units += entry.second.unit_extras.size();
    }
    return stats;
  }

 private:
  using Table =
      std::unordered_map<PlayerKey, PlayerExtra, PlayerKeyHash, PlayerKeyEqual>;

  // 取/建某玩家的顶层条目（调用方必须已持独占锁）。
  PlayerExtra& PlayerFor(std::int32_t player_id) {
    const PlayerKey key{player_id};
    PlayerExtra& player = table_[key];
    if (player.player_id != player_id) {
      player = PlayerExtra{};
      player.player_id = player_id;
    }
    return player;
  }

  // 两张子表都空时擦除玩家条目（调用方必须已持独占锁）。
  void ErasePlayerIfBothEmpty(Table::iterator player_it) {
    if (player_it->second.city_extras.empty() &&
        player_it->second.unit_extras.empty()) {
      table_.erase(player_it);
    }
  }

  mutable std::shared_mutex mutex_;
  Table table_;
};

// 进程内单例。注意：静态局部量是 **per-DLL** 的——loader 与各插件各持一份表。
// 同一插件 DLL 内的多个效果模块（city-yield / strength）共用同一张表；其它插件若需
// 共享，必须改由 loader 侧暴露服务（当前不需要）。
inline PlayerExtraTable& PlayerExtras() {
  static PlayerExtraTable table;
  return table;
}

} // namespace ykkz000::extra
