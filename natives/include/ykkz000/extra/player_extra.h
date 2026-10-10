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
/// @note Model: the extension side table mirrors the engine's own structure instead of a lazy
///       effect-scoped map. Every one-to-many mapping the engine owns (player -> cities, player ->
///       units) is reproduced here with **object pointers** as keys:
///         top-level: Player::Instance* (stable for the lifetime of a game context);
///         child:     City::Instance* / Unit::Instance* (stable while the object lives).
///       An `AExtra` therefore organizes its `BExtra` children by pointer, exactly like the engine
///       mapping (see natives/AGENTS.md "扩展数据结构规范"). Entries are created when the object is
///       constructed and dropped when it is destroyed, so an extension exists for every live object
///       rather than only for objects some effect happened to touch.
/// @note Object identity (pointer / id / owner) is recycled by the engine: a destroyed object's
///       pointer or (owner, id) key can be reused. Building the table on the **pointer** plus
///       dropping the record from the City/Unit destructor hooks removes the reliance on (owner, id)
///       that previously survived capture/move and corrupted the save/load stream.
/// @note PlayerExtra contains unordered_map and child pointers, so it is **not a copyable type**.
///       The read hot path always goes through single-entry queries (FindCity snapshot /
///       FindUnitStrength scalar); **never** copy a whole PlayerExtra or a whole child table -- that
///       would copy "every city and unit of the player".
/// @note The child tables hold `unique_ptr` values so that an `AExtra` owns its `BExtra` by pointer,
///       matching the engine's one-to-many mapping (player -> cities / player -> units) while
///       keeping RAII ownership. The `Edit*` paths create the child on first access; the `Erase*` /
///       `Drop*` paths release it.
namespace ykkz000::extra {

/// @brief Per-city extension data (key = City::Instance pointer, stored in PlayerExtra::cities).
/// @note percent[y]: the "percent per citizen" for that yield in this city, stored as FixedPoint<16>,
///       where 0x10000 == +1.0%/citizen (i.e. 1.0 means "+1% per citizen").
///       The read path folds it into the engine's modifier units as (percent[y] * population) >> 8:
///       the engine's modifier sub-object accumulates "percentage points" as FixedPoint<8>
///       (1.0 == +1%), final yield = base * (1 + modifier / 25600) (25600 == +100%). Example:
///       Amount=5 (i.e. +5% per citizen), population=10: percent = 5 * 0x10000 = 327680,
///       delta = (327680 * 10) >> 8 = 12800 == +50% (12800 / 25600). So FixedPoint<16> "percentage
///       points" under this representation are exactly lossless with (x * pop) >> 8.
/// @note Aggregation semantics: Apply adds (+=), Remove subtracts (-=), independent of call order or
///       instance count (repeated instances with the same parameters are no longer ambiguous).
/// @note per_suzerain_percent[y]: the "percent per suzerain city" for that yield in this city, in the
///       same units as percent[y] (FixedPoint<16>, 0x10000 == +1.0%/suzerain). The suzerain count is
///       a runtime variable, so it is not multiplied at write time; instead the read path folds
///       (per_suzerain_percent[y] x current suzerain count) >> 8 into the engine's modifier units, so
///       a suzerain-count change is naturally followed by the next read.
/// @note city_id / owner_id / player are diagnostic/identity snapshots only: lookup is by the City
///       pointer, so a capture that rewrites owner_id never loses the entry. `player` records the
///       owning Player::Instance* so the entry can be reparented to the new owner's child table.
/// @note hydrated is in-memory only (not persisted): it records that the values were already seeded
///       from the city's persisted AutoVariables, so the (expensive) AutoVariable read runs once per
///       object after a load instead of on every read.
struct CityExtra {
  std::int32_t city_id = -1;    ///< City::Instance +0xA8
  std::int32_t owner_id = -1;   ///< City::Instance +0xD8 (PlayerTypes)
  void* player = nullptr;       ///< Owning Player::Instance* (top-level key)
  bool hydrated = false;        ///< In-memory only: side-table values seeded from AutoVariables
  std::int32_t yield_count = 0; ///< Valid length (<= civ6::kMaxYields), shared by both arrays
  std::array<std::int32_t, civ6::kMaxYields> percent{};
  std::array<std::int32_t, civ6::kMaxYields> per_suzerain_percent{};
};

static_assert(std::is_standard_layout_v<CityExtra>);
static_assert(std::is_trivially_copyable_v<CityExtra>);

/// @brief Per-unit extension data (key = Unit::Instance pointer, stored in PlayerExtra::units).
/// @note strength_per_suzerain: the sum of the "per suzerain city" flat strength values of the
///       effect instances applying to this unit, in the same units as
///       AdjustPlayerStrengthModifier::amount (+0x40). The write-point hook books
///       strength_per_suzerain x current suzerain count.
/// @note instances: an upsert map keyed by the effect object (instance = self) that guarantees
///       idempotence -- re-applying the same instance only overwrites, never double-adds (the engine
///       replaying Apply does not inflate); strength_per_suzerain always equals the sum of the
///       values.
/// @note unit_id / owner_id / player are diagnostic/identity snapshots only; lookup is by the Unit
///       pointer.
/// @note Reserved: later per-unit quantities are appended here (keep append-only, never change the
///       offsets of existing fields).
struct UnitExtra {
  std::int32_t unit_id = -1;              ///< Unit::Instance +0xB0
  std::int32_t owner_id = -1;             ///< Unit::Instance +0x128 (PlayerTypes)
  void* player = nullptr;                 ///< Owning Player::Instance* (top-level key)
  std::int32_t strength_per_suzerain = 0; ///< Sum over instances
  /// @note Reserved (unused): the aggregate restored from the unit's persisted AutoVariable after a
  ///       load. The strength template is replayed by the engine after a load, so the side table is
  ///       rebuilt from instances and this field is no longer read or written. Kept for the
  ///       append-only field layout; do not remove or reorder.
  std::int32_t baseline = 0;
  std::unordered_map<void*, std::int32_t> instances; ///< key = the effect object self
};

/// @brief Per-player extension data (key = Player::Instance*), holding the two child tables.
struct PlayerExtra {
  void* player = nullptr;       ///< Player::Instance* (top-level key)
  std::int32_t player_id = -1;  ///< Player::Instance +0xD8 (PlayerTypes; diagnostic)
  std::unordered_map<void*, std::unique_ptr<CityExtra>> cities; ///< key = City::Instance*
  std::unordered_map<void*, std::unique_ptr<UnitExtra>> units;  ///< key = Unit::Instance*
};

/// @brief Thread-safe side table: a single shared_mutex covers the top level, both child tables, and
///   the pointer indexes (writes exclusive, reads shared).
/// @note Entries exist for every live City/Unit once `EditCity`/`EditUnit` (or the construct-time
///       `Ensure*`) has run; `EraseCity`/`EraseUnit` drop them on destruction. The pointer indexes
///       (`city_index_`/`unit_index_`) let every query resolve an entry from the object pointer
///       alone, independent of the owner bucket, so a capture that changes the owner can never make
///       an existing entry unreachable.
class PlayerExtraTable {
 public:
  /// @brief Construct-time hook: create the city's extension entry for a freshly constructed object,
  ///   replacing any stale entry left by a recycled pointer (a destructor that was missed).
  /// @param[in] player Owning Player::Instance* (may be null when the owner is not assigned yet).
  /// @param[in] city City::Instance pointer (the lookup key).
  /// @param[in] city_id City id snapshot (diagnostic; -1 when not assigned yet).
  /// @param[in] owner_id Owner snapshot (diagnostic; -1 when not assigned yet).
  /// @note Unlike `EditCity`, this resets the entry: a brand-new object must never inherit the values
  ///       recorded for a previous object that reused the same address.
  void EnsureCity(void* player, void* city, std::int32_t city_id, std::int32_t owner_id) {
    if (city == nullptr) {
      return;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    EraseCityLocked(city);
    (void)FindOrCreateCity(player, city, city_id, owner_id);
  }

  /// @brief Write path: get/create and edit a single city entry.
  /// @param[in] player Owning Player::Instance* (entry is reparented when it changed).
  /// @param[in] city City::Instance pointer (the lookup key).
  /// @param[in] city_id City id snapshot (diagnostic).
  /// @param[in] owner_id Owner snapshot (diagnostic).
  /// @param[in] fn An edit callback taking CityExtra&.
  /// @note The callback form (rather than returning a reference) keeps the whole read-modify-write
  ///       under the lock, avoiding races with the shared-lock read path.
  template <typename Fn>
  void EditCity(void* player, void* city, std::int32_t city_id, std::int32_t owner_id, Fn&& fn) {
    if (city == nullptr) {
      return;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    CityExtra* entry = FindOrCreateCity(player, city, city_id, owner_id);
    if (entry != nullptr) {
      fn(*entry);
    }
  }

  /// @brief Read-only query (hot path): a snapshot of a single city entry by object pointer.
  /// @param[in] city City::Instance pointer.
  /// @param[out] out On a hit, receives a snapshot of the entry (CityExtra is trivially copyable, no
  ///   heap allocation).
  /// @return true on a hit, otherwise false (out is unchanged).
  bool FindCity(void* city, CityExtra& out) const {
    if (city == nullptr) {
      return false;
    }
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto index_it = city_index_.find(city);
    if (index_it == city_index_.end() || index_it->second == nullptr) {
      return false;
    }
    out = *index_it->second;
    return true;
  }

  /// @brief Reparent a city entry into a new owner's child table (capture / owner change).
  /// @param[in] player The new owning Player::Instance*.
  /// @param[in] city City::Instance pointer.
  /// @param[in] owner_id The new owner snapshot (diagnostic).
  /// @note No-op when the entry does not exist or is already under `player`.
  void ReparentCity(void* player, void* city, std::int32_t owner_id) {
    if (city == nullptr || player == nullptr) {
      return;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto index_it = city_index_.find(city);
    if (index_it == city_index_.end() || index_it->second == nullptr) {
      return;
    }
    CityExtra* entry = index_it->second;
    entry->owner_id = owner_id;
    MoveCityToPlayer(player, city, entry);
  }

  /// @brief Unconditionally erase a city entry (the engine destroyed the city), then erase the
  ///   player entry when both child tables become empty.
  /// @param[in] city City::Instance pointer.
  /// @note Called on the destruction path, where the object is gone regardless of the values still
  ///       recorded in the entry.
  void EraseCity(void* city) {
    if (city == nullptr) {
      return;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    EraseCityLocked(city);
  }

  /// @brief Construct-time hook: create the unit's extension entry for a freshly constructed object,
  ///   replacing any stale entry left by a recycled pointer.
  /// @param[in] player Owning Player::Instance* (may be null when the owner is not assigned yet).
  /// @param[in] unit Unit::Instance pointer (the lookup key).
  /// @param[in] unit_id Unit id snapshot (diagnostic; -1 when not assigned yet).
  /// @param[in] owner_id Owner snapshot (diagnostic; -1 when not assigned yet).
  void EnsureUnit(void* player, void* unit, std::int32_t unit_id, std::int32_t owner_id) {
    if (unit == nullptr) {
      return;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    EraseUnitLocked(unit);
    (void)FindOrCreateUnit(player, unit, unit_id, owner_id);
  }

  /// @brief Write path: get/create and edit a single unit entry.
  /// @param[in] player Owning Player::Instance* (entry is reparented when it changed).
  /// @param[in] unit Unit::Instance pointer (the lookup key).
  /// @param[in] unit_id Unit id snapshot (diagnostic).
  /// @param[in] owner_id Owner snapshot (diagnostic).
  /// @param[in] fn An edit callback taking UnitExtra&.
  template <typename Fn>
  void EditUnit(void* player, void* unit, std::int32_t unit_id, std::int32_t owner_id, Fn&& fn) {
    if (unit == nullptr) {
      return;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    UnitExtra* entry = FindOrCreateUnit(player, unit, unit_id, owner_id);
    if (entry != nullptr) {
      fn(*entry);
    }
  }

  /// @brief Read-only query (hot path): only the aggregate scalar of a unit by object pointer.
  /// @param[in] unit Unit::Instance pointer.
  /// @return The aggregate scalar.
  /// @note **Does not copy the instances map** (avoids a heap allocation on every read). Both a
  ///       missing entry and a zero aggregate return 0, and the caller falls back to the template
  ///       Amount accordingly.
  std::int32_t FindUnitStrength(void* unit) const {
    if (unit == nullptr) {
      return 0;
    }
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto index_it = unit_index_.find(unit);
    if (index_it == unit_index_.end() || index_it->second == nullptr) {
      return 0;
    }
    return index_it->second->strength_per_suzerain;
  }

  /// @brief Reparent a unit entry into a new owner's child table (unit transfer).
  /// @param[in] player The new owning Player::Instance*.
  /// @param[in] unit Unit::Instance pointer.
  /// @param[in] owner_id The new owner snapshot (diagnostic).
  void ReparentUnit(void* player, void* unit, std::int32_t owner_id) {
    if (unit == nullptr || player == nullptr) {
      return;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto index_it = unit_index_.find(unit);
    if (index_it == unit_index_.end() || index_it->second == nullptr) {
      return;
    }
    UnitExtra* entry = index_it->second;
    entry->owner_id = owner_id;
    MoveUnitToPlayer(player, unit, entry);
  }

  /// @brief Unconditionally erase a unit entry (the engine destroyed the unit), then erase the
  ///   player entry when both child tables become empty.
  /// @param[in] unit Unit::Instance pointer.
  void EraseUnit(void* unit) {
    if (unit == nullptr) {
      return;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    EraseUnitLocked(unit);
  }

  /// @brief Ensure a player's top-level entry exists (lazily called; there is no player-construct
  ///   entry point yet, so a player bucket is created on first city/unit association).
  /// @param[in] player Player::Instance*.
  /// @param[in] player_id Player type (diagnostic).
  void EnsurePlayer(void* player, std::int32_t player_id) {
    if (player == nullptr) {
      return;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    PlayerExtra& bucket = PlayerFor(player);
    bucket.player_id = player_id;
  }

  /// @brief Drop a player's whole top-level entry (player eliminated). Removes the pointer indexes
  ///   of every owned city/unit so no dangling index remains.
  /// @param[in] player Player::Instance*.
  void DropPlayer(void* player) {
    if (player == nullptr) {
      return;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto player_it = table_.find(player);
    if (player_it == table_.end()) {
      return;
    }
    for (const auto& entry : player_it->second.cities) {
      city_index_.erase(entry.first);
    }
    for (const auto& entry : player_it->second.units) {
      unit_index_.erase(entry.first);
    }
    table_.erase(player_it);
  }

  /// @brief Clear the whole side table.
  void Clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    table_.clear();
    city_index_.clear();
    unit_index_.clear();
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
  using Table = std::unordered_map<void*, PlayerExtra>;

  /// @brief Get/create a player's top-level entry.
  /// @note The caller must already hold the exclusive lock.
  PlayerExtra& PlayerFor(void* player) {
    PlayerExtra& bucket = table_[player];
    if (bucket.player != player) {
      bucket = PlayerExtra{};
      bucket.player = player;
    }
    return bucket;
  }

  /// @brief Erase a city entry and its index record.
  /// @note The caller must already hold the exclusive lock.
  void EraseCityLocked(void* city) {
    const auto index_it = city_index_.find(city);
    if (index_it == city_index_.end()) {
      return;
    }
    CityExtra* entry = index_it->second;
    city_index_.erase(index_it);
    if (entry != nullptr && entry->player != nullptr) {
      const auto player_it = table_.find(entry->player);
      if (player_it != table_.end()) {
        player_it->second.cities.erase(city);
        ErasePlayerIfEmpty(player_it);
      }
    }
  }

  /// @brief Erase a unit entry and its index record.
  /// @note The caller must already hold the exclusive lock.
  void EraseUnitLocked(void* unit) {
    const auto index_it = unit_index_.find(unit);
    if (index_it == unit_index_.end()) {
      return;
    }
    UnitExtra* entry = index_it->second;
    unit_index_.erase(index_it);
    if (entry != nullptr && entry->player != nullptr) {
      const auto player_it = table_.find(entry->player);
      if (player_it != table_.end()) {
        player_it->second.units.erase(unit);
        ErasePlayerIfEmpty(player_it);
      }
    }
  }

  /// @brief Find a city entry by pointer (creating it under `player` when absent).
  /// @note The caller must already hold the exclusive lock.
  CityExtra* FindOrCreateCity(void* player, void* city, std::int32_t city_id,
                              std::int32_t owner_id) {
    const auto index_it = city_index_.find(city);
    if (index_it != city_index_.end() && index_it->second != nullptr) {
      CityExtra* entry = index_it->second;
      entry->city_id = city_id;
      entry->owner_id = owner_id;
      MoveCityToPlayer(player, city, entry);
      return entry;
    }
    if (player == nullptr) {
      return nullptr;
    }
    PlayerExtra& bucket = PlayerFor(player);
    bucket.player_id = owner_id;
    auto owned = std::make_unique<CityExtra>();
    owned->city_id = city_id;
    owned->owner_id = owner_id;
    owned->player = player;
    CityExtra* raw = owned.get();
    bucket.cities.emplace(city, std::move(owned));
    city_index_.emplace(city, raw);
    return raw;
  }

  /// @brief Find a unit entry by pointer (creating it under `player` when absent).
  /// @note The caller must already hold the exclusive lock.
  UnitExtra* FindOrCreateUnit(void* player, void* unit, std::int32_t unit_id,
                              std::int32_t owner_id) {
    const auto index_it = unit_index_.find(unit);
    if (index_it != unit_index_.end() && index_it->second != nullptr) {
      UnitExtra* entry = index_it->second;
      entry->unit_id = unit_id;
      entry->owner_id = owner_id;
      MoveUnitToPlayer(player, unit, entry);
      return entry;
    }
    if (player == nullptr) {
      return nullptr;
    }
    PlayerExtra& bucket = PlayerFor(player);
    bucket.player_id = owner_id;
    auto owned = std::make_unique<UnitExtra>();
    owned->unit_id = unit_id;
    owned->owner_id = owner_id;
    owned->player = player;
    UnitExtra* raw = owned.get();
    bucket.units.emplace(unit, std::move(owned));
    unit_index_.emplace(unit, raw);
    return raw;
  }

  /// @brief Move a city's owning unique_ptr to another player's child table (node transfer keeps
  ///   the pointee address and therefore the raw pointer index valid).
  /// @note The caller must already hold the exclusive lock.
  void MoveCityToPlayer(void* player, void* city, CityExtra* entry) {
    if (player == nullptr || entry == nullptr || entry->player == player) {
      return;
    }
    void* const old_player = entry->player;
    if (old_player == nullptr) {
      return;
    }
    const auto old_it = table_.find(old_player);
    if (old_it == table_.end()) {
      return;
    }
    auto node = old_it->second.cities.extract(city);
    if (node.empty()) {
      return;
    }
    // Erase the old bucket (if it is now empty) *before* inserting into the destination: the insert
    // may rehash `table_` and invalidate this iterator.
    if (old_it->second.cities.empty() && old_it->second.units.empty()) {
      table_.erase(old_it);
    }
    PlayerExtra& dst = PlayerFor(player);
    dst.player_id = entry->owner_id;
    dst.cities.insert(std::move(node));
    entry->player = player;
  }

  /// @brief Unit counterpart of MoveCityToPlayer.
  /// @note The caller must already hold the exclusive lock.
  void MoveUnitToPlayer(void* player, void* unit, UnitExtra* entry) {
    if (player == nullptr || entry == nullptr || entry->player == player) {
      return;
    }
    void* const old_player = entry->player;
    if (old_player == nullptr) {
      return;
    }
    const auto old_it = table_.find(old_player);
    if (old_it == table_.end()) {
      return;
    }
    auto node = old_it->second.units.extract(unit);
    if (node.empty()) {
      return;
    }
    if (old_it->second.cities.empty() && old_it->second.units.empty()) {
      table_.erase(old_it);
    }
    PlayerExtra& dst = PlayerFor(player);
    dst.player_id = entry->owner_id;
    dst.units.insert(std::move(node));
    entry->player = player;
  }

  /// @brief Erase the player entry when both child tables are empty.
  /// @note The caller must already hold the exclusive lock. Invalidates `player_it`.
  void ErasePlayerIfEmpty(Table::iterator player_it) {
    if (player_it->second.cities.empty() && player_it->second.units.empty()) {
      table_.erase(player_it);
    }
  }

  mutable std::shared_mutex mutex_;
  Table table_;
  std::unordered_map<void*, CityExtra*> city_index_; ///< City::Instance* -> owned entry (raw)
  std::unordered_map<void*, UnitExtra*> unit_index_; ///< Unit::Instance* -> owned entry (raw)
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
