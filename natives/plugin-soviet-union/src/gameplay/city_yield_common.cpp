#include <ykkz000/plugin/city_yield_common.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <shared_mutex>
#include <unordered_map>

#include <ykkz000/civ6/city.h>
#include <ykkz000/civ6/effect.h>

#include <ykkz000/plugin/engine_access.h>
#include <ykkz000/plugin/extra_persistence.h>

namespace ykkz000::plugin {
namespace {

// The engine's own entry City::Instance::ChangeYieldModifier(YieldTypes, int) (published RVA
// 0x131CF0). Calling it with a zero delta leaves the value array unchanged but goes through the
// engine's "yield changed" notification/invalidation dispatch (0x5FB890).
using ChangeYieldModifierFn = void (*)(void* city, int yield, int delta);

// Side-table write generation: incremented after Apply/Remove to invalidate TLS caches (write
// paths complete inside their own locks).
std::atomic<std::uint64_t> kTableGeneration{0};

// Read-path TLS cache: consecutive lookups of the same city hit directly, avoiding a shared-lock
// table lookup on every call.
// The cache identity is the **city object pointer** (the side-table key): a recycled pointer that is
// reused by a new city produces a different generation at the destruction/edit point and is
// invalidated naturally, and a capture that only changes the owner keeps the same entry.
struct TlsCache {
  std::uint64_t generation = 0;
  void* city = nullptr;
  extra::CityExtra extra{};
  bool valid = false;
};
thread_local TlsCache kCache;

// Suzerain-count cache: CountSuzerainsOfPlayer walks the player vector, whereas the read happens
// on the city-yield hot path.
// The TTL approach is stale by at most one TTL for a short time right after a suzerain change;
// the write points (Apply/Remove) and context switches do not pass through here, so the TTL is
// the fallback.
struct SuzerainCountEntry {
  int count = -1;
  std::chrono::steady_clock::time_point expiry{};
};
constexpr auto kSuzerainCountTtl = std::chrono::milliseconds(250);
std::shared_mutex kSuzerainMutex;
std::unordered_map<std::int32_t, SuzerainCountEntry> kSuzerainCache;

#if defined(_DEBUG)
// Rate-limited read-path diagnostic: hit/miss, key, and the current generation (the negative cache
// is a suspect during save/load).
void LogCityLookup(const CityRef& ref, std::uint64_t generation, const char* outcome,
                   const extra::CityExtra* extra) {
  static std::atomic<long> kCount{0};
  const long n = ++kCount;
  if (n > 32 && (n % 4096) != 0) {
    return;
  }
  const std::int32_t percent = extra != nullptr ? extra->percent[0] : 0;
  const std::int32_t per_suzerain = extra != nullptr ? extra->per_suzerain_percent[0] : 0;
  const int yield_count = extra != nullptr ? extra->yield_count : -1;
  LogDebugF("city-yield: lookup player=%d city=%d outcome=%s generation=%llu pct[0]=%d "
            "suz[0]=%d yield_count=%d",
            ref.player_id, ref.city_id, outcome,
            static_cast<unsigned long long>(generation), percent, per_suzerain, yield_count);
}

// Local diagnostic clock, anchored on this translation unit's first use (the Plugin's persistence
// layer keeps its own anchor; the two are not claimed to be a shared origin).
long long DebugElapsedMs() {
  static const auto start = std::chrono::steady_clock::now();
  return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start).count());
}

std::atomic<long> kHydrateCalls{0};

// Cold-path (non-TLS) timing: reports the elapsed microseconds every 128 lookups, and snapshots the
// side-table size every 1024 to watch for unbounded growth.
void LogCityLookupSlow(const CityRef& ref, const char* outcome,
                       std::chrono::steady_clock::time_point start) {
  static std::atomic<long> kCount{0};
  const long n = ++kCount;
  if (n > 16 && (n % 128) != 0) {
    return;
  }
  const long long elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - start).count();
  LogDebugF("city-yield: slow #%ld t_ms=%lld player=%d city=%d outcome=%s elapsed_us=%lld", n,
            DebugElapsedMs(), ref.player_id, ref.city_id, outcome, elapsed_us);
  if ((n % 1024) == 0) {
    const extra::PlayerExtraTable::Stats stats = extra::PlayerExtras().CountStats();
    LogDebugF("city-yield: extra table players=%zu cities=%zu units=%zu", stats.players,
              stats.cities, stats.units);
  }
}
#endif

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

const extra::CityExtra* LookupCityExtraForCity(void* city, const CityRef& ref) {
  if (city == nullptr) {
    return nullptr;
  }
  const std::uint64_t generation = kTableGeneration.load(std::memory_order_acquire);
  if (kCache.generation == generation && kCache.city == city) {
#if defined(_DEBUG)
    LogCityLookup(ref, generation, kCache.valid ? "tls-hit" : "tls-miss",
                  kCache.valid ? &kCache.extra : nullptr);
#endif
    return kCache.valid ? &kCache.extra : nullptr;
  }
#if defined(_DEBUG)
  const auto slow_start = std::chrono::steady_clock::now();
#endif
  // Slow path: make sure the entry exists and was seeded from the persisted AutoVariables (this also
  // reparents the entry when the owner changed), then cache the outcome.
  EnsureCityExtraHydrated(city, ref);
  const std::uint64_t generation_after = kTableGeneration.load(std::memory_order_acquire);
  kCache.generation = generation_after;
  kCache.city = city;
  extra::CityExtra found;
  if (extra::PlayerExtras().FindCity(city, found)) {
    kCache.extra = found;
    kCache.valid = true;
#if defined(_DEBUG)
    LogCityLookup(ref, generation_after, "table-hit", &kCache.extra);
    LogCityLookupSlow(ref, "table-hit", slow_start);
#endif
    return &kCache.extra;
  }
  kCache.extra = extra::CityExtra{};
  kCache.valid = false;
#if defined(_DEBUG)
  LogCityLookup(ref, generation_after, "miss", nullptr);
  LogCityLookupSlow(ref, "miss", slow_start);
#endif
  return nullptr;
}

void EnsureCityExtraHydrated(void* city, const CityRef& ref) {
  if (city == nullptr || ref.player_id < 0 || ref.city_id < 0) {
    return;
  }
  // Fast path: the entry exists, was seeded, and still belongs to the current owner.
  extra::CityExtra existing;
  if (extra::PlayerExtras().FindCity(city, existing) && existing.hydrated &&
      existing.owner_id == ref.player_id) {
    return;
  }
  // The entry is keyed by the object pointer; the owner only decides which child table it lives in.
  void* const player = PlayerForOwnerId(ref.player_id);
  if (player == nullptr) {
    // The owner could not be resolved yet (e.g. very early construction): leave the entry untouched
    // and retry on a later read/write rather than creating it under the wrong owner.
    return;
  }
  std::array<std::int32_t, civ6::kMaxYields> percent{};
  std::array<std::int32_t, civ6::kMaxYields> per_suzerain{};
#if defined(_DEBUG)
  const long hydrate_n = ++kHydrateCalls;
  const bool hydrate_report = hydrate_n <= 16 || (hydrate_n % 128) == 0;
  const auto load_start = std::chrono::steady_clock::now();
#endif
  const bool loaded = LoadCityValues(city, percent.data(), per_suzerain.data());
  bool seeded = false;
  extra::PlayerExtras().EditCity(
      player, city, ref.city_id, ref.player_id, [&](extra::CityExtra& entry) {
        if (entry.hydrated) {
          // Already seeded (only the owner changed): EditCity has reparented it; nothing else to do.
          return;
        }
        if (loaded) {
          entry.percent = percent;
          entry.per_suzerain_percent = per_suzerain;
          std::int32_t count = 0;
          for (std::size_t i = 0; i < civ6::kMaxYields; ++i) {
            if (percent[i] != 0 || per_suzerain[i] != 0) {
              count = static_cast<std::int32_t>(i) + 1;
            }
          }
          entry.yield_count = count;
          seeded = true;
        }
        // Mark as seeded even when nothing was stored, so the AutoVariable read does not run again.
        entry.hydrated = true;
      });
  if (seeded) {
    static std::atomic<long> kSeedCount{0};
    const long n = ++kSeedCount;
    if (n <= 8 || (n % 4096) == 0) {
      LogInfoF("city-yield: hydrated player=%d city=%d pct[1]=%d suz[1]=%d", ref.player_id,
               ref.city_id, percent[1], per_suzerain[1]);
    }
    InvalidateCityExtraSnapshotCache();
  }
#if defined(_DEBUG)
  if (hydrate_report) {
    const long long load_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - load_start)
                                  .count();
    LogDebugF("city-yield: hydrate #%ld t_ms=%lld player=%d city=%d load_us=%lld seeded=%d",
              hydrate_n, DebugElapsedMs(), ref.player_id, ref.city_id, load_us,
              seeded ? 1 : 0);
  }
#endif
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
  // Bumping the generation invalidates other threads' TLS snapshots too (clearing only this
  // thread's TLS would not cover them).
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
  // Prefer matching by +0xD8 (index != player type); on failure fall back to taking the real
  // player at that index.
  void* player = PlayerForOwnerId(player_id);
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
