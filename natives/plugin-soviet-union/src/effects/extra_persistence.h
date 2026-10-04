#pragma once

#include <cstdint>

/// @file extra_persistence.h
/// @brief Persistence of mod side-table values through engine FAutoVariable members.
/// @note The side tables (city yield percentages, unit strength per suzerain) live only in memory,
///       so they are lost when the game reconstructs its objects on load. This layer registers a
///       custom FAutoVariable on each City/Unit's own FAutoArchive during construction, then hooks
///       the engine's own City/Unit save/load entries to invoke the variables' descriptor value
///       methods: the custom values are appended after the engine's members on save and read back in
///       the same order on load. The effect modules mirror their side table into the variables after
///       every write and hydrate from them before the first write (see the effect modules).
namespace ykkz000::plugin {

/// @brief Installs the City/Unit constructor hooks (idempotent).
/// @return true when persistence is active (all engine entries resolved and hooks installed).
/// @note Safe to call repeatedly on every context creation; when any required engine entry is
///       missing the call logs once and returns false, and the effect modules keep their current
///       in-memory-only behavior.
[[nodiscard]] bool EnsurePersistenceHooks();

/// @brief Context destruction / new game: remove the hooks, release the registered variables, and
///        drop the pointer maps.
void ShutdownPersistence();

/// @brief Plugin unload: remove the hooks and drop the pointer maps without touching engine memory
///        (GameCore may already be unloading).
void ResetPersistenceForUnload();

/// @brief Copies the city yield percentage arrays into the city's AutoVariables.
/// @param[in] city City instance.
/// @param[in] percent per-citizen array of kMaxYields entries.
/// @param[in] per_suzerain per-suzerain array of kMaxYields entries.
/// @note No-op when persistence is inactive or the city has no registered variables.
void PersistCityValues(void* city, const std::int32_t* percent, const std::int32_t* per_suzerain);

/// @brief Reads the city yield percentage arrays from the city's AutoVariables.
/// @param[in] city City instance.
/// @param[out] percent_out Receives kMaxYields entries (zeroed first).
/// @param[out] per_suzerain_out Receives kMaxYields entries (zeroed first).
/// @return true when the city has registered variables and at least one stored value is non-zero.
[[nodiscard]] bool LoadCityValues(void* city, std::int32_t* percent_out,
                                  std::int32_t* per_suzerain_out);

/// @brief Copies the unit strength-per-suzerain aggregate into the unit's AutoVariable.
void PersistUnitStrength(void* unit, std::int32_t value);

/// @brief Reads the unit strength-per-suzerain aggregate from the unit's AutoVariable.
/// @param[out] out Receives the stored value (set to 0 on failure).
/// @return true when the unit has a registered variable and the stored value is non-zero.
[[nodiscard]] bool LoadUnitStrength(void* unit, std::int32_t& out);

} // namespace ykkz000::plugin
