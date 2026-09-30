#pragma once

#include <cstdint>
#include <vector>

#include <ykkz000/extra/player_extra.h>

// 城市产出修正效果的公共工具：被 city-yield-per-population（持有 CalculateYield
// hook）与 city-yield-per-suzerain（只聚合、无 hook）两个模块共用，避免两处实现
// 解析/键/缓存失效逻辑时产生细微分歧。
namespace ykkz000::plugin {

// 效果对象上的一个 (YieldType, Amount) 条目。语义由调用方决定：
//   每市民模块把 Amount 当“每市民百分比”，每宗主模块当“每宗主百分比”。
struct EffectEntry {
  int yield_type = 0;
  int amount = 0;
};

// 由城市实例取侧表键（读 City+0xD8 玩家 / City+0xA8 城市 id，做合理性校验）。
// 顶层键即该城市所属玩家（PlayerExtras 以玩家为单位隔离重复文明/领袖）。
struct CityRef {
  std::int32_t player_id = -1;
  std::int32_t city_id = -1;
};

// percent[]/per_suzerain_percent[] 的编码：Amount(%) × 0x10000 =>
// 0x10000 == +1%（每市民或每宗主）。读取时 (值 × 因子) >> 8 得到引擎修正单位。
inline constexpr std::int32_t kPercentUnit = 0x10000;

// 单条 Amount 的合理上限（防御解析错误导致的异常值）。
inline constexpr int kMaxPlausibleAmount = 100000;

// 读取效果对象上的 (YieldType, Amount) 条目，与 Effects::AdjustCityYieldModifier
// 的遍历结构一致；条目不可信时返回 false。
[[nodiscard]] bool ReadEffectEntries(void* self, std::vector<EffectEntry>& out);

// 由城市实例解析侧表键（读 +0xD8 玩家 / +0xA8 城市 id）。
[[nodiscard]] bool CityRefOf(const void* city, CityRef& ref);

// 清 city+0x1950 中某产出的缓存有效标志，迫使引擎下次读取时重算（越界产出忽略）。
void InvalidateCityYieldCache(void* city, int yield_type);
// 以 0 增量调用 ChangeYieldModifier：不改数值数组，但走引擎的“产出已变化”通知/失效分发。
void NotifyCityYieldChanged(void* city, int yield_type);
// 对一组条目做上面两步（越界产出跳过）；Apply/Remove 后调用以刷新引擎缓存。
void InvalidateAndNotifyCityYield(void* city, const std::vector<EffectEntry>& entries);

// 读路径 TLS 快照：同键的连续查询直接命中；未命中再查表并按需回填。
// 无条目返回 nullptr。以侧表键（玩家 id + 城市 id）为缓存标识，避免用城市指针。
[[nodiscard]] const extra::CityExtra* LookupCityExtra(std::int32_t player_id,
                                                      std::int32_t city_id);

// 侧表写后失效：写代际自增（令所有线程的 TLS 快照失效）并清本线程 TLS。
void InvalidateCityExtraSnapshotCache();

// 侧表写代际（诊断用）：非 0 表示 Apply/Remove 或上下文清表曾发生。
[[nodiscard]] std::uint64_t CityExtraSnapshotGeneration();

// 上下文销毁/新局：清 TLS 快照与宗主数缓存。
void ResetCityYieldCommonCaches();

// 带 TTL 缓存的宗主数（CountSuzerainsOfPlayer 会遍历玩家向量，而本读取是热路径）。
// 不可信时返回 -1；0 是合法值。TTL 内的滞后最多一个 TTL。
[[nodiscard]] int SuzerainCountForPlayer(std::int32_t player_id);

} // namespace ykkz000::plugin
