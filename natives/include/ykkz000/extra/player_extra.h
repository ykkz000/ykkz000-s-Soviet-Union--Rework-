#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <type_traits>
#include <unordered_map>

#include <ykkz000/civ6/common.h> // civ6::kMaxYields
#include <ykkz000/civ6/player.h> // PlayerTypes / PlayerTypeIndex

/// @file player_extra.h
/// @brief Overlay data and side tables for this mod (not part of the engine; shared by the loader
///   and each plugin).
/// @note Directory semantics:
///       civ6/  = mirror of engine object layouts (describes only fields/types that really exist in
///                the engine, and introduces no mod data);
///       extra/ = overlay data and side tables for this mod (not part of the engine; shared by the
///                loader and each plugin).
/// @note The single top-level extension side table: `PlayerExtras()`, keyed by player_id (the
///       PlayerTypes at Player::Instance +0xD8). Each player entry holds two child tables:
///       cities: key = city_id (City::Instance +0xA8, unique within a player);
///       units: key = unit_id (Unit::Instance +0xB0, unique within a player).
///       The top-level key is the player, so two players with a duplicated civilization/leader are
///       naturally isolated; cleanup is also player-scoped (ClearPlayer drops all of that player's
///       city/unit entries at once), with no need to scan for stale entries by id.
/// @note PlayerExtra contains unordered_map and child pointers, so it is **not a copyable type**.
///       The read hot path always goes through single-entry queries (FindCity snapshot /
///       FindUnitStrength scalar); **never** copy a whole PlayerExtra or a whole child table -- that
///       would copy "every city and unit of the player".
/// @note The child tables hold `unique_ptr` values so that an `AExtra` organizes its `BExtra` by
///       pointer, matching the engine's one-to-many mapping (player -> cities / player -> units)
///       while keeping RAII ownership. The `Edit*` paths create the child on first access; the
///       erase/clear paths release it.
namespace ykkz000::extra {

/// @brief Per-city extension data (key = city_id, stored in PlayerExtra::cities).
/// @note percent[y]: the "percent per citizen" for that yield in this city, stored as FixedPoint<16>,
///       where 0x10000 == +1.0%/citizen (i.e. 1.0 means "+1% per citizen").
///       The read path folds it into the engine's modifier units as (percent[y] * population) >> 8:
///       the engine's modifier sub-object accumulates "percentage points" as FixedPoint<8>
///       (1.0 == +1%), final yield = base * (1 + modifier / 25600) (25600 == +100%). Example:
///       Amount=5 (i.e. +5% per citizen), population=10: percent = 5 * 0x10000 = 327680,
///       delta = (327680 * 10) >> 8 = 12800 == +50% (12800 / 25600). So FixedPoint<16> "percentage
///       points" under this representation are exactly lossless with (x * pop) >> 8.
/// @note Aggregation semantics: Apply adds (+=), Remove subtracts (-=), independent of call order or
///       instance count (repeated instances with the same parameters are no longer ambiguous); an
///       all-zero vector is erased to avoid leftovers.
/// @note per_suzerain_percent[y]: the "percent per suzerain city" for that yield in this city, in the
///       same units as percent[y] (FixedPoint<16>, 0x10000 == +1.0%/suzerain). The suzerain count is
///       a runtime variable, so it is not multiplied at write time; instead the read path folds
///       (per_suzerain_percent[y] x current suzerain count) >> 8 into the engine's modifier units, so
///       a suzerain-count change is naturally followed by the next read.
/// @note owner_id is a redundant check only: the read path re-reads City+0xD8 and compares, guarding
///       against pointer recycling or a change of owner.
struct CityExtra {
  std::int32_t city_id = -1;    ///< City::Instance +0xA8
  std::int32_t owner_id = -1;   ///< City::Instance +0xD8 (PlayerTypes)
  std::int32_t yield_count = 0; ///< Valid length (<= civ6::kMaxYields), shared by both arrays
  std::array<std::int32_t, civ6::kMaxYields> percent{};
  std::array<std::int32_t, civ6::kMaxYields> per_suzerain_percent{};
};

static_assert(std::is_standard_layout_v<CityExtra>);
static_assert(std::is_trivially_copyable_v<CityExtra>);

/// @brief Per-unit extension data (key = unit_id, stored in PlayerExtra::units).
/// @note strength_per_suzerain: the sum of the "per suzerain city" flat strength values of the
///       effect instances applying to this unit, in the same units as
///       AdjustPlayerStrengthModifier::amount (+0x40). The write-point hook books
///       strength_per_suzerain x current suzerain count.
/// @note instances: an upsert map keyed by the effect object (instance = self) that guarantees
///       idempotence -- re-applying the same instance only overwrites, never double-adds (the engine
///       replaying Apply/Remove does not inflate); strength_per_suzerain always equals the sum of the
///       values.
/// @note Reserved: later per-unit quantities are appended here (keep append-only, never change the
///       offsets of existing fields).
struct UnitExtra {
  std::int32_t unit_id = -1;             ///< Unit::Instance +0xB0
  std::int32_t strength_per_suzerain = 0; ///< Sum over instances
  std::unordered_map<void*, std::int32_t> instances; ///< key = the effect object self
};

/// @brief Per-player extension data (key = player_id), holding the two child tables.
struct PlayerExtra {
  std::int32_t player_id = -1; ///< Player::Instance +0xD8 (PlayerTypes)
  std::unordered_map<std::int32_t, std::unique_ptr<CityExtra>> cities; ///< key = city_id
  std::unordered_map<std::int32_t, std::unique_ptr<UnitExtra>> units;  ///< key = unit_id
};

/// @brief Top-level side-table key: player id.
struct PlayerKey {
  std::int32_t player_id = -1; ///< Player id (Player::Instance +0xD8, PlayerTypes)
};

/// @brief Hash functor for PlayerKey.
struct PlayerKeyHash {
  /// @brief Hash a PlayerKey.
  /// @param[in] key The key to hash.
  /// @return The mixed hash value.
  std::size_t operator()(const PlayerKey& key) const noexcept {
    // Same style as CityKeyHash: mix then expand, to avoid low-bit clustering.
    std::uint64_t value = static_cast<std::uint32_t>(key.player_id);
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    return static_cast<std::size_t>(value);
  }
};

/// @brief Equality functor for PlayerKey.
struct PlayerKeyEqual {
  /// @brief Compare two PlayerKeys for equality.
  /// @param[in] a Left key.
  /// @param[in] b Right key.
  /// @return true when both keys have the same player id.
  bool operator()(const PlayerKey& a, const PlayerKey& b) const noexcept {
    return a.player_id == b.player_id;
  }
};

/// @brief Thread-safe side table: a single shared_mutex covers the top level and both child tables
///   (writes exclusive, reads shared).
class PlayerExtraTable {
 public:
  /// @brief Write path: get/create and edit a player's top-level entry under the exclusive lock.
  /// @param[in] key The player key.
  /// @param[in] fn An edit callback taking PlayerExtra&.
  /// @note The callback form (rather than returning a reference) keeps the whole read-modify-write
  ///       under the lock, avoiding races with the shared-lock read path.
  template <typename Fn>
  void EditPlayer(const PlayerKey& key, Fn&& fn) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    fn(PlayerFor(key.player_id));
  }

  /// @brief Write path: edit a single city entry of a player (created from city_id/owner_id if it
  ///   does not exist).
  /// @param[in] player_id Player id.
  /// @param[in] city_id City id.
  /// @param[in] owner_id The city owner (redundant check, see CityExtra::owner_id).
  /// @param[in] fn An edit callback taking CityExtra&.
  template <typename Fn>
  void EditCity(std::int32_t player_id, std::int32_t city_id,
                std::int32_t owner_id, Fn&& fn) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    PlayerExtra& player = PlayerFor(player_id);
    std::unique_ptr<CityExtra>& slot = player.cities[city_id];
    if (slot == nullptr || slot->city_id != city_id || slot->owner_id != owner_id) {
      slot = std::make_unique<CityExtra>();
      slot->city_id = city_id;
      slot->owner_id = owner_id;
    }
    fn(*slot);
  }

  /// @brief Write path: edit a single unit entry of a player (created if it does not exist).
  /// @param[in] player_id Player id.
  /// @param[in] unit_id Unit id.
  /// @param[in] fn An edit callback taking UnitExtra&.
  template <typename Fn>
  void EditUnit(std::int32_t player_id, std::int32_t unit_id, Fn&& fn) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    PlayerExtra& player = PlayerFor(player_id);
    std::unique_ptr<UnitExtra>& slot = player.units[unit_id];
    if (slot == nullptr || slot->unit_id != unit_id) {
      slot = std::make_unique<UnitExtra>();
      slot->unit_id = unit_id;
    }
    fn(*slot);
  }

  /// @brief Read-only query (hot path): a snapshot of a single city entry.
  /// @param[in] player_id Player id.
  /// @param[in] city_id City id.
  /// @param[out] out On a hit, receives a snapshot of the entry (CityExtra is trivially copyable, no
  ///   heap allocation).
  /// @return true on a hit, otherwise false (out is unchanged).
  bool FindCity(std::int32_t player_id, std::int32_t city_id,
                CityExtra& out) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto player_it = table_.find(PlayerKey{player_id});
    if (player_it == table_.end()) {
      return false;
    }
    const auto city_it = player_it->second.cities.find(city_id);
    if (city_it == player_it->second.cities.end()) {
      return false;
    }
    out = *city_it->second;
    return true;
  }

  /// @brief Read-only query (hot path): only the aggregate scalar of a unit.
  /// @param[in] player_id Player id.
  /// @param[in] unit_id Unit id.
  /// @return The aggregate scalar.
  /// @note **Does not copy the instances map** (avoids a heap allocation on every read). Both a
  ///       missing entry and a zero aggregate return 0, and the caller falls back to the template
  ///       Amount accordingly.
  std::int32_t FindUnitStrength(std::int32_t player_id,
                                std::int32_t unit_id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto player_it = table_.find(PlayerKey{player_id});
    if (player_it == table_.end()) {
      return 0;
    }
    const auto unit_it = player_it->second.units.find(unit_id);
    if (unit_it == player_it->second.units.end()) {
      return 0;
    }
    return unit_it->second->strength_per_suzerain;
  }

  /// @brief Erase the city entry once both arrays are all zero; erase the player entry too when both
  ///   child tables become empty, to avoid leftover empty shells.
  /// @param[in] player_id Player id.
  /// @param[in] city_id City id.
  void EraseIfEmptyCity(std::int32_t player_id, std::int32_t city_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto player_it = table_.find(PlayerKey{player_id});
    if (player_it == table_.end()) {
      return;
    }
    PlayerExtra& player = player_it->second;
    const auto city_it = player.cities.find(city_id);
    if (city_it == player.cities.end()) {
      return;
    }
    const CityExtra& city = *city_it->second;
    for (const std::int32_t value : city.percent) {
      if (value != 0) {
        return;
      }
    }
    for (const std::int32_t value : city.per_suzerain_percent) {
      if (value != 0) {
        return;
      }
    }
    player.cities.erase(city_it);
    ErasePlayerIfBothEmpty(player_it);
  }

  /// @brief Erase the unit entry once there are no instances and the aggregate is zero; erase the
  ///   player entry too when both child tables become empty.
  /// @param[in] player_id Player id.
  /// @param[in] unit_id Unit id.
  void EraseIfEmptyUnit(std::int32_t player_id, std::int32_t unit_id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto player_it = table_.find(PlayerKey{player_id});
    if (player_it == table_.end()) {
      return;
    }
    PlayerExtra& player = player_it->second;
    const auto unit_it = player.units.find(unit_id);
    if (unit_it == player.units.end()) {
      return;
    }
    if (!unit_it->second->instances.empty() ||
        unit_it->second->strength_per_suzerain != 0) {
      return;
    }
    player.units.erase(unit_it);
    ErasePlayerIfBothEmpty(player_it);
  }

  /// @brief Player died / was eliminated: drop all of that player's city/unit entries.
  /// @param[in] key The player key.
  void ClearPlayer(const PlayerKey& key) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    table_.erase(key);
  }

  /// @brief Clear the whole side table.
  void Clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    table_.clear();
  }

  /// @brief Diagnostics: one-shot table size statistics.
  /// @note Only called on warning/self-check paths, not on the hot path.
  struct Stats {
    std::size_t players = 0; ///< Number of player entries
    std::size_t cities = 0;  ///< Number of city entries
    std::size_t units = 0;   ///< Number of unit entries
  };

  /// @brief Count the current side-table size.
  /// @return Player/city/unit entry counts.
  Stats CountStats() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    Stats stats;
    stats.players = table_.size();
    for (const auto& entry : table_) {
      stats.cities += entry.second.cities.size();
      stats.units += entry.second.units.size();
    }
    return stats;
  }

 private:
  using Table =
      std::unordered_map<PlayerKey, PlayerExtra, PlayerKeyHash, PlayerKeyEqual>;

  /// @brief Get/create a player's top-level entry.
  /// @note The caller must already hold the exclusive lock.
  PlayerExtra& PlayerFor(std::int32_t player_id) {
    const PlayerKey key{player_id};
    PlayerExtra& player = table_[key];
    if (player.player_id != player_id) {
      player = PlayerExtra{};
      player.player_id = player_id;
    }
    return player;
  }

  /// @brief Erase the player entry when both child tables are empty.
  /// @note The caller must already hold the exclusive lock.
  void ErasePlayerIfBothEmpty(Table::iterator player_it) {
    if (player_it->second.cities.empty() &&
        player_it->second.units.empty()) {
      table_.erase(player_it);
    }
  }

  mutable std::shared_mutex mutex_;
  Table table_;
};

/// @brief In-process singleton side table.
/// @return The process-wide PlayerExtraTable reference.
/// @note The static local is **per-DLL**: the loader and each plugin hold their own table. Multiple
///       effect modules within the same plugin DLL (city-yield / strength) share one table; other
///       plugins would have to share through a loader-side service if needed (not required today).
inline PlayerExtraTable& PlayerExtras() {
  static PlayerExtraTable table;
  return table;
}

} // namespace ykkz000::extra
