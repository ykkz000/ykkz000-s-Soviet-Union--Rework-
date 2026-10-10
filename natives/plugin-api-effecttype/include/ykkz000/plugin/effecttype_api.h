#pragma once

#include <cstddef>
#include <cstdint>

#include <ykkz000/bridge/host.h>
#include <ykkz000/plugin/effect.h>

/// @file effecttype_api.h
/// @brief C ABI of the ykkz000_plugin_api_effecttype plugin.
/// @note The API plugin is itself a plugin: the loader initializes it before its consumers (their
///       manifests declare the dependency), so by the time a consumer's GetPlugin runs the API is
///       ready and its exports are usable. The surface is a single versioned, append-only POD table
///       of C function pointers; no STL types, exceptions, or allocator-owned objects cross the DLL
///       boundary.
namespace ykkz000::plugin {

/// @brief API revision requested through GetEffectTypeApi. A consumer passes this value; the API
///        returns null when it cannot satisfy the request.
inline constexpr std::uint32_t kEffectTypeApiVersion = 1;

/// @brief Runtime defensive bounds.
/// @note A sudden jump in entry count or an absurd population/player index is usually a symptom of
///       an invalid self/city pointer; in that case it is better to skip the operation than to
///       corrupt a garbage address.
inline constexpr int kMaxEffectEntries = 64;
inline constexpr int kMaxPlausiblePopulation = 100000;
inline constexpr int kMaxPlausiblePlayerIndex = 255;
inline constexpr int kMaxPlausibleSuzerainCount = 128;

/// @brief Global context captured by the API: host services and engine entry points.
struct PluginContext {
  const bridge::Host* host = nullptr;         ///< Host service table
  const bridge::EngineApi* engine = nullptr;  ///< Engine entry-point table
};

/// @brief Versioned C ABI table exported by the effecttype API plugin.
/// @note Append-only: existing fields never change meaning, new fields may be appended, and the
///       version gate lets a consumer detect an older API.
struct EffectTypeApi {
  std::uint32_t version; ///< Must equal kEffectTypeApiVersion

  /// @brief Capture the host context used by every other entry here.
  void (*set_context)(const bridge::Host* host);
  /// @brief Read the captured context.
  const PluginContext* (*get_context)();

  /// @brief Register one effect descriptor (thin wrapper over host->registerEffectType).
  /// @return 0 on success, non-zero on failure.
  int (*register_effect)(const bridge::EffectDesc* desc);
  /// @brief Register every effect in a manifest array (calls describe then register_effect).
  /// @return 0 on success, non-zero on failure.
  int (*register_effects)(const Effect* const* effects, std::uint32_t count);

  /// @brief Emit one log line.
  void (*log)(int level, const char* message);
  /// @brief Format and emit one log line (printf style).
  void (*logf)(int level, const char* format, ...);

  /// @brief Validated readability test.
  int (*is_readable)(const void* address, std::size_t bytes);
  /// @brief Engine-object candidate test.
  int (*is_candidate_object)(const void* pointer);

  /// @brief Player-vector range (begin/end of void*).
  int (*get_player_vector)(void** begin, void** end);
  /// @brief Whether a pointer is a member of the player vector.
  int (*is_real_player)(const void* candidate);
  /// @brief Exact player lookup by the player type at +0xD8.
  void* (*player_by_id)(int player_id);
  /// @brief Player by vector index.
  void* (*player_at_index)(int index);
  /// @brief Player resolution by owner id (by +0xD8, then by index).
  void* (*player_for_owner_id)(int player_id);
  /// @brief Player resolution from an object carrying a player-type field.
  void* (*resolve_player_from_object)(const void* object, const char** via);
  /// @brief Count the city-states suzerained by one player; -1 when untrustworthy.
  int (*count_suzerains_of_player)(void* player);
};

/// @brief Export entry point: obtain the effecttype API table.
/// @param[in] version Requested API revision (kEffectTypeApiVersion).
/// @return The API table, or null when the request cannot be satisfied.
} // namespace ykkz000::plugin

extern "C" const ykkz000::plugin::EffectTypeApi* GetEffectTypeApi(std::uint32_t version);

namespace ykkz000::plugin {

/// @brief Cached API accessor (resolved once per DLL).
/// @return The effecttype API table.
inline const EffectTypeApi* Api() {
  static const EffectTypeApi* api = GetEffectTypeApi(kEffectTypeApiVersion);
  return api;
}

} // namespace ykkz000::plugin
