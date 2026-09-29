#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <shared_mutex>
#include <type_traits>
#include <unordered_map>

#include <ykkz000/civ6/city.h>   // City::Instance（键取自 +0xA8 / +0xD8）
#include <ykkz000/civ6/common.h> // civ6::kMaxYields

// 目录语义：
//   civ6/  = 引擎对象布局的镜像（只描述引擎真实存在的字段/类型，不引入本模组数据）。
//   extra/ = 本模组叠加的数据与侧表（不属于引擎，可被 loader 与各插件共享）。
//
// 城市扩展数据侧表（共享头：loader 与各插件均可 #include）。
//
// 用途：为“每市民百分比”一类效果提供一个与调用顺序/实例个数无关的聚合值。
// Apply/Remove 只做对称的 += / -=（幂等、可交换），真正的“乘人口”发生在引擎
// 读取城市产出的路径上（见 CityYield 模块的 CalculateYield hook）——因此人口变化
// 无需任何 hook，产出重算时自然取用当前人口。
//
// 键 = (city_id, owner_id)：含所属玩家 ⇒ 城市易主天然分离；键来自游戏状态而非
// 指针 ⇒ 无指针回收/悬垂问题。
namespace ykkz000::extra {

// 每座城市的扩展数据（键 = 城市 ID + 所属玩家）。
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
struct CityExtra {
  std::int32_t city_id = -1;    // City::Instance + 0xA8
  std::int32_t owner_id = -1;   // City::Instance + 0xD8（PlayerTypes）
  std::int32_t yield_count = 0; // 有效长度（≤ civ6::kMaxYields）
  std::array<std::int32_t, civ6::kMaxYields> percent{};
};

static_assert(std::is_standard_layout_v<CityExtra>);
static_assert(std::is_trivially_copyable_v<CityExtra>);

struct CityKey {
  std::int32_t city_id = -1;
  std::int32_t owner_id = -1;
};

struct CityKeyHash {
  std::size_t operator()(const CityKey& key) const noexcept {
    std::uint64_t value =
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.city_id)) << 32) |
        static_cast<std::uint32_t>(key.owner_id);
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    return static_cast<std::size_t>(value);
  }
};

struct CityKeyEqual {
  bool operator()(const CityKey& a, const CityKey& b) const noexcept {
    return a.city_id == b.city_id && a.owner_id == b.owner_id;
  }
};

// 线程安全侧表：读路径只读（共享锁），写路径（Apply/Remove）独占。
class CityExtraTable {
 public:
  // 写路径：在独占锁内取/建并编辑，fn 收到该城市的 CityExtra&。
  // 回调形式（而非返回引用）保证“读改写”全程持锁，避免与共享锁读路径竞争。
  template <typename Fn>
  void Edit(const CityKey& key, Fn&& fn) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    CityExtra& extra = table_[key];
    if (extra.city_id != key.city_id || extra.owner_id != key.owner_id) {
      extra = CityExtra{};
      extra.city_id = key.city_id;
      extra.owner_id = key.owner_id;
    }
    fn(extra);
  }

  // 只读查询（热路径）：拷出快照，释放锁后调用方自行使用。
  bool Find(const CityKey& key, CityExtra& out) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto it = table_.find(key);
    if (it == table_.end()) {
      return false;
    }
    out = it->second;
    return true;
  }

  // 向量全零后擦除该键，避免残留空条目。
  void EraseIfEmpty(const CityKey& key) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto it = table_.find(key);
    if (it == table_.end()) {
      return;
    }
    for (const std::int32_t value : it->second.percent) {
      if (value != 0) {
        return;
      }
    }
    table_.erase(it);
  }

  void Clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    table_.clear();
  }

 private:
  mutable std::shared_mutex mutex_;
  std::unordered_map<CityKey, CityExtra, CityKeyHash, CityKeyEqual> table_;
};

// 进程内单例。注意：静态局部量是 **per-DLL** 的——loader 与各插件各持一份表。
// 若将来多个模块需要共享同一张表，必须改由 loader 侧暴露服务（当前仅城市产出模块
// 一个使用者，暂不处理）。
inline CityExtraTable& CityExtras() {
  static CityExtraTable table;
  return table;
}

} // namespace ykkz000::extra
