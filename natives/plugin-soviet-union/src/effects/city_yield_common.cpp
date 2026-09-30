#include "city_yield_common.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include <ykkz000/civ6/city.h>
#include <ykkz000/civ6/effect.h>

#include "engine_access.h"

namespace ykkz000::plugin {
namespace {

// 引擎自身入口 City::Instance::ChangeYieldModifier(YieldTypes, int)（发布 RVA 0x131CF0）。
// 以 0 增量调用不改数值数组，但会走引擎的“产出已变化”通知/失效分发（0x5FB890）。
using ChangeYieldModifierFn = void (*)(void* city, int yield, int delta);

// 侧表写代际：Apply/Remove 后自增，令 TLS 缓存失效（写路径在各自锁内完成）。
std::atomic<std::uint64_t> kTableGeneration{0};

// 读路径 TLS 缓存：同键的连续查询直接命中，避免每次加共享锁查表。
// 以侧表键（玩家 id + 城市 id，而非城市指针）为缓存标识：城市指针被回收复用时键
// 不同，天然失效。
struct TlsCache {
  std::uint64_t generation = 0;
  std::int32_t player_id = -1;
  std::int32_t city_id = -1;
  extra::CityExtra extra{};
  bool valid = false;
};
thread_local TlsCache kCache;

// 宗主数缓存：CountSuzerainsOfPlayer 会遍历玩家向量，而读取发生在城市产出热路径。
// TTL 方案在“刚变更宗主”的短时间内最多滞后一个 TTL；写入点（Apply/Remove）与上下文
// 切换均不经过此处，改由 TTL 兜底。
struct SuzerainCountEntry {
  int count = -1;
  std::chrono::steady_clock::time_point expiry{};
};
constexpr auto kSuzerainCountTtl = std::chrono::milliseconds(250);
std::shared_mutex kSuzerainMutex;
std::unordered_map<std::int32_t, SuzerainCountEntry> kSuzerainCache;

} // namespace

bool ReadEffectEntries(void* self, std::vector<EffectEntry>& out) {
  const int entry_count =
      TryReadOr(self, &civ6::CityYieldModifierEffect::entry_count, std::int32_t{0});
  const int amount_count =
      TryReadOr(self, &civ6::CityYieldModifierEffect::amount_count, std::int32_t{0});
  int* yields = nullptr;
  int* amounts = nullptr;
  (void)TryRead(self, &civ6::CityYieldModifierEffect::yields, yields);
  (void)TryRead(self, &civ6::CityYieldModifierEffect::amounts, amounts);
  if (entry_count <= 0 || entry_count > kMaxEffectEntries || amount_count <= 0 ||
      amount_count > kMaxEffectEntries || yields == nullptr || amounts == nullptr) {
    return false;
  }
  const int count = entry_count < amount_count ? entry_count : amount_count;
  out.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    out.push_back(EffectEntry{yields[i], amounts[i]});
  }
  return !out.empty();
}

bool CityRefOf(const void* city, CityRef& ref) {
  const std::int32_t city_id =
      TryReadOr(city, &civ6::City::Instance::city_id, std::int32_t{-1});
  const std::int32_t owner = TryReadOr(city, &civ6::City::Instance::owner, std::int32_t{-1});
  if (city_id < 0 || owner < 0 || owner > kMaxPlausiblePlayerIndex) {
    return false;
  }
  ref.player_id = owner;
  ref.city_id = city_id;
  return true;
}

void InvalidateCityYieldCache(void* city, int yield_type) {
  if (city == nullptr || yield_type < 0 ||
      yield_type >= static_cast<int>(civ6::kMaxYields)) {
    return;
  }
  void* cache = nullptr;
  if (!TryRead(city, &civ6::City::Instance::yield_cache, cache) || cache == nullptr) {
    return;
  }
  const std::size_t offset =
      offsetof(civ6::YieldCacheEntry, valid) +
      static_cast<std::size_t>(yield_type) * sizeof(civ6::YieldCacheEntry);
  const std::uint8_t invalid = 0;
  (void)TryWriteAt(cache, offset, invalid);
}

void NotifyCityYieldChanged(void* city, int yield_type) {
  const bridge::EngineApi* engine = Context().engine;
  if (engine == nullptr || engine->changeYieldModifier == nullptr) {
    return;
  }
  const auto change =
      reinterpret_cast<ChangeYieldModifierFn>(engine->changeYieldModifier);
  change(city, yield_type, 0);
}

void InvalidateAndNotifyCityYield(void* city, const std::vector<EffectEntry>& entries) {
  for (const EffectEntry& entry : entries) {
    if (entry.yield_type < 0 ||
        entry.yield_type >= static_cast<int>(civ6::kMaxYields)) {
      continue;
    }
    InvalidateCityYieldCache(city, entry.yield_type);
    NotifyCityYieldChanged(city, entry.yield_type);
  }
}

const extra::CityExtra* LookupCityExtra(std::int32_t player_id, std::int32_t city_id) {
  const std::uint64_t generation = kTableGeneration.load(std::memory_order_acquire);
  if (kCache.valid && kCache.generation == generation &&
      kCache.player_id == player_id && kCache.city_id == city_id) {
    return &kCache.extra;
  }
  extra::CityExtra found;
  if (!extra::PlayerExtras().FindCity(player_id, city_id, found)) {
    kCache.valid = false;
    return nullptr;
  }
  kCache.generation = generation;
  kCache.player_id = player_id;
  kCache.city_id = city_id;
  kCache.extra = found;
  kCache.valid = true;
  return &kCache.extra;
}

void InvalidateCityExtraSnapshotCache() {
  kTableGeneration.fetch_add(1, std::memory_order_release);
  kCache = TlsCache{};
}

std::uint64_t CityExtraSnapshotGeneration() {
  return kTableGeneration.load(std::memory_order_relaxed);
}

void ResetCityYieldCommonCaches() {
  {
    std::unique_lock<std::shared_mutex> lock(kSuzerainMutex);
    kSuzerainCache.clear();
  }
  // 代际自增使其它线程的 TLS 快照一并失效（仅清本线程 TLS 不足以覆盖其它线程）。
  InvalidateCityExtraSnapshotCache();
}

int SuzerainCountForPlayer(std::int32_t player_id) {
  if (player_id < 0 || player_id > kMaxPlausiblePlayerIndex) {
    return -1;
  }
  const auto now = std::chrono::steady_clock::now();
  {
    std::shared_lock<std::shared_mutex> lock(kSuzerainMutex);
    const auto it = kSuzerainCache.find(player_id);
    if (it != kSuzerainCache.end() && now < it->second.expiry) {
      return it->second.count;
    }
  }
  // 优先按 +0xD8 匹配（下标≠玩家类型），失败再退回按下标取真玩家。
  void* player = PlayerById(player_id);
  if (player == nullptr || !IsRealPlayer(player)) {
    player = PlayerAtIndex(player_id);
  }
  if (player == nullptr) {
    return -1;
  }
  const int count = CountSuzerainsOfPlayer(player);
  if (count < 0) {
    return -1;
  }
  {
    std::unique_lock<std::shared_mutex> lock(kSuzerainMutex);
    kSuzerainCache[player_id] = SuzerainCountEntry{count, now + kSuzerainCountTtl};
  }
  return count;
}

} // namespace ykkz000::plugin
