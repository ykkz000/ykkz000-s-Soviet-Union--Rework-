#pragma once

#include <cstdint>

/// @file persistence_api.h
/// @brief C ABI of the ykkz000_plugin_api_persistence plugin.
/// @note The API plugin is itself a plugin: consumers declare it as a manifest dependency so the
///       loader initializes it first. It provides variable-description-driven persistence of custom
///       int-vector AutoVariables on engine City objects plus City/Unit lifecycle notifications, all
///       behind a versioned, append-only POD table of C function pointers. No STL types, exceptions,
///       or allocator-owned objects cross the DLL boundary.
namespace ykkz000::plugin {

/// @brief API revision requested through GetPersistenceApi.
inline constexpr std::uint32_t kPersistenceApiVersion = 1;

/// @brief Object lifecycle callback signature (user context plus the object pointer).
using ObjectLifecycleFn = void (*)(void* user, void* object);

/// @brief Versioned C ABI table exported by the persistence API plugin.
/// @note Append-only: existing fields never change meaning, new fields may be appended.
struct PersistenceApi {
  std::uint32_t version; ///< Must equal kPersistenceApiVersion

  /// @brief Declare an int-vector AutoVariable attached to every City at construction.
  /// @param[in] name Stable variable name (must never change once shipped; resolved by name on load).
  /// @param[in] capacity Element count of the vector.
  /// @return 1 on success, 0 on failure.
  /// @note Must be called before a game context creates Cities (typically from GetPlugin).
  int (*declare_city_int_vector)(const char* name, std::uint32_t capacity);

  /// @brief Store values into a declared city variable.
  /// @param[in] city City instance.
  /// @param[in] name Declared variable name.
  /// @param[in] values Source values.
  /// @param[in] count Number of values (clamped to the declared capacity).
  void (*store_city_int_vector)(void* city, const char* name, const std::int32_t* values,
                                std::uint32_t count);

  /// @brief Load values from a declared city variable (out zeroed first).
  /// @param[in] city City instance.
  /// @param[in] name Declared variable name.
  /// @param[out] out Receives up to `capacity` values.
  /// @param[in] capacity Size of out.
  /// @return Non-zero when the city has the variable and at least one stored value is non-zero.
  int (*load_city_int_vector)(void* city, const char* name, std::int32_t* out,
                              std::uint32_t capacity);

  /// @brief Register consumer callbacks for engine City/Unit lifecycle events.
  /// @param[in] user Opaque context forwarded to each callback.
  /// @param[in] city_created Called after a City is constructed (may be null).
  /// @param[in] city_destroyed Called after a City is destroyed (may be null).
  /// @param[in] unit_created Called after a Unit is constructed (may be null).
  /// @param[in] unit_destroyed Called after a Unit is destroyed (may be null).
  /// @return 1 on success, 0 on failure.
  int (*register_object_lifecycle)(void* user, ObjectLifecycleFn city_created,
                                   ObjectLifecycleFn city_destroyed, ObjectLifecycleFn unit_created,
                                   ObjectLifecycleFn unit_destroyed);
};

} // namespace ykkz000::plugin

/// @brief Export entry point: obtain the persistence API table.
/// @param[in] version Requested API revision (kPersistenceApiVersion).
/// @return The API table, or null when the request cannot be satisfied.
extern "C" const ykkz000::plugin::PersistenceApi* GetPersistenceApi(std::uint32_t version);

namespace ykkz000::plugin {

/// @brief Cached API accessor (resolved once per DLL).
/// @return The persistence API table.
inline const PersistenceApi* Persistence() {
  static const PersistenceApi* api = GetPersistenceApi(kPersistenceApiVersion);
  return api;
}

} // namespace ykkz000::plugin
