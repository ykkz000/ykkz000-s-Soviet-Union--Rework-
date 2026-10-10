#pragma once

#include <cstdint>
#include <vector>

#include <ykkz000/extra/player_extra.h>

/// @file city_yield_common.h
/// @brief Shared utilities for city-yield modifier effects.
/// @note Shared by the city-yield-per-population module (which owns the CalculateYield hook) and
///       the city-yield-per-suzerain module (aggregation only, no hook), so that parsing/key/
///       cache-invalidation logic cannot drift subtly between two implementations.
namespace ykkz000::plugin {

/// @brief One (YieldType, Amount) entry on an effect object.
/// @note The caller decides the semantics: the per-population module treats Amount as
///       "percent per citizen" and the per-suzerain module as "percent per suzerain".
struct EffectEntry {
  int yield_type = 0;
  int amount = 0;
};

/// @brief Side-table key derived from a city instance (reads City+0xD8 player / City+0xA8 city
///        id and sanity-checks them).
/// @note The key is **not** the table key anymore (entries are keyed by the City pointer); it only
///       carries the current owner id for suzerain-count resolution and the city id for diagnostics.
struct CityRef {
  std::int32_t player_id = -1;
  std::int32_t city_id = -1;
};

/// @brief Encoding of percent[]/per_suzerain_percent[]: Amount(%) × 0x10000.
/// @note 0x10000 == +1% (per citizen or per suzerain). Reading as (value × factor) >> 8 yields
///       engine modifier units.
inline constexpr std::int32_t kPercentUnit = 0x10000;

/// @brief Plausibility cap for a single Amount (defends against parse errors producing absurd
///        values).
inline constexpr int kMaxPlausibleAmount = 100000;

/// @brief Persistence variable names, declared through the persistence API plugin.
/// @note Stable once shipped: the engine archive resolves variables by name on load.
inline constexpr char kCityPercentVarName[] = "m_aYkkz000YieldPercentPerPopulation";
inline constexpr char kCityPerSuzerainVarName[] = "m_aYkkz000YieldPercentPerSuzerain";

/// @brief Copies the city yield percentage arrays into the city's persisted AutoVariables.
/// @param[in] city City instance.
/// @param[in] percent per-citizen array of kMaxYields entries.
/// @param[in] per_suzerain per-suzerain array of kMaxYields entries.
/// @note No-op when persistence is inactive or the city has no registered variables.
void PersistCityValues(void* city, const std::int32_t* percent, const std::int32_t* per_suzerain);

/// @brief Reads the city yield percentage arrays from the city's persisted AutoVariables.
/// @param[in] city City instance.
/// @param[out] percent_out Receives kMaxYields entries (zeroed first).
/// @param[out] per_suzerain_out Receives kMaxYields entries (zeroed first).
/// @return true when the city has registered variables and at least one stored value is non-zero.
[[nodiscard]] bool LoadCityValues(void* city, std::int32_t* percent_out,
                                  std::int32_t* per_suzerain_out);

/// @brief Reads the (YieldType, Amount) entries on an effect object.
/// @param[in] self Effect object.
/// @param[out] out Receives the entry list.
/// @return true on success; false when the entries are untrustworthy.
/// @note Matches the traversal structure of Effects::AdjustCityYieldModifier.
[[nodiscard]] bool ReadEffectEntries(void* self, std::vector<EffectEntry>& out);

/// @brief Resolves the side-table key from a city instance (reads +0xD8 player / +0xA8 city id).
/// @param[in] city City instance.
/// @param[out] ref Receives the side-table key.
/// @return true on success.
[[nodiscard]] bool CityRefOf(const void* city, CityRef& ref);

/// @brief Clears one yield's cache-valid flag in city+0x1950, forcing the engine to recompute on
///        its next read.
/// @param[in] city City instance.
/// @param[in] yield_type Yield type.
/// @note Out-of-range yields are ignored.
void InvalidateCityYieldCache(void* city, int yield_type);
/// @brief Calls ChangeYieldModifier with a zero delta.
/// @param[in] city City instance.
/// @param[in] yield_type Yield type.
/// @note Does not change the value array, but goes through the engine's "yield changed"
///       notification/invalidation dispatch.
void NotifyCityYieldChanged(void* city, int yield_type);
/// @brief Performs both steps above for a set of entries (out-of-range yields are skipped).
/// @param[in] city City instance.
/// @param[in] entries Entry list.
/// @note Call after Apply/Remove to refresh the engine caches.
void InvalidateAndNotifyCityYield(void* city, const std::vector<EffectEntry>& entries);

/// @brief Read-path TLS snapshot: consecutive lookups of the same city hit directly; on a miss it
///        ensures the entry exists, seeds it once from the city's persisted AutoVariables (see
///        extra_persistence.h), and caches the outcome (including a negative result).
/// @param[in] city City instance (the side-table key).
/// @param[in] ref Owner/city-id snapshot (used for owner resolution, diagnostics, and hydrate).
/// @return Side-table entry pointer on hit, nullptr when there is no entry.
/// @note The cache identity is the city object pointer, so a capture that only changes the owner
///       keeps the same result; a write (Apply/Remove) bumps the generation and invalidates it.
[[nodiscard]] const extra::CityExtra* LookupCityExtraForCity(void* city, const CityRef& ref);

/// @brief Seeds the side-table entry from the city's persisted AutoVariables when the entry does
///        not exist yet or has not been seeded (e.g. right after a savegame load, where the engine
///        does not replay Apply).
/// @param[in] city City instance.
/// @param[in] ref Owner/city-id snapshot resolved from the city.
/// @note No-op when persistence is inactive or the entry is already seeded for the current owner.
///       Call before the first EditCity of a city (in the read path and before Apply/Remove) so a
///       later incremental Apply adds on top of the restored values instead of resetting them.
void EnsureCityExtraHydrated(void* city, const CityRef& ref);

/// @brief Post-write side-table invalidation: bumps the write generation (invalidating all
///        threads' TLS snapshots) and clears this thread's TLS.
void InvalidateCityExtraSnapshotCache();

/// @brief Gets the side-table write generation (for diagnostics).
/// @return Generation number; non-zero means Apply/Remove or a context table clear has occurred.
[[nodiscard]] std::uint64_t CityExtraSnapshotGeneration();

/// @brief Context destruction/new game: clears the TLS snapshot and the suzerain-count cache.
void ResetCityYieldCommonCaches();

/// @brief TTL-cached suzerain count.
/// @param[in] player_id Player id.
/// @return Suzerain count; -1 when untrustworthy (0 is a valid value).
/// @note CountSuzerainsOfPlayer walks the player vector, whereas this read is on a hot path.
///       Within the TTL the staleness is at most one TTL.
[[nodiscard]] int SuzerainCountForPlayer(std::int32_t player_id);

} // namespace ykkz000::plugin
