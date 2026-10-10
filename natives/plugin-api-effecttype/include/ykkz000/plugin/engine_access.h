#pragma once

#include <cstddef>
#include <cstdint>

#include <ykkz000/bridge/host.h>
#include <ykkz000/bridge/log.h>
#include <ykkz000/civ6/city.h>
#include <ykkz000/civ6/game_manager.h>
#include <ykkz000/civ6/player.h>
#include <ykkz000/civ6/unit.h>
#include <ykkz000/plugin/effecttype_api.h>

/// @file engine_access.h
/// @brief Consumer-side engine access layer over the effecttype API.
/// @note The heavy lifting (host/context access, validated reads/writes, player resolution) lives in
///       the ykkz000_plugin_api_effecttype plugin; this header provides the type-safe inline
///       wrappers and member-access templates consumers use. Every engine read must be validated
///       (host->readField / isReadableRegion); engine pointers are always treated as untrusted.
namespace ykkz000::plugin {

/// @brief Gets the plugin context captured through the effecttype API.
/// @return The PluginContext reference (an empty context before SetContext).
[[nodiscard]] inline const PluginContext& Context() {
  static const PluginContext kEmpty{};
  const PluginContext* context = Api()->get_context();
  return context != nullptr ? *context : kEmpty;
}

/// @brief Sets the plugin global context.
/// @param[in] host Host service table.
inline void SetContext(const bridge::Host* host) { Api()->set_context(host); }

// -- Level wrappers --
// Zero-overhead level macros. TRACE is never used; DEBUG is compiled out entirely unless _DEBUG is
// defined (so its formatting arguments are not evaluated). Plugin logs are forwarded through the
// API, which relays them to host->log where the Loader applies the runtime filter.
#define LogTrace(message) ((void)0)
#define LogTraceF(...) ((void)0)
#if defined(_DEBUG)
#define LogDebug(message) \
  (::ykkz000::plugin::Api()->log(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kDebug), \
                                 (message)))
#define LogDebugF(...)                                                                          \
  (::ykkz000::plugin::Api()->logf(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kDebug), \
                                  __VA_ARGS__))
#else
#define LogDebug(message) ((void)0)
#define LogDebugF(...) ((void)0)
#endif
#define LogInfo(message) \
  (::ykkz000::plugin::Api()->log(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kInfo), \
                                 (message)))
#define LogInfoF(...)                                                                          \
  (::ykkz000::plugin::Api()->logf(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kInfo), \
                                  __VA_ARGS__))
#define LogWarn(message) \
  (::ykkz000::plugin::Api()->log(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kWarning), \
                                 (message)))
#define LogWarnF(...)                                                                          \
  (::ykkz000::plugin::Api()->logf(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kWarning), \
                                  __VA_ARGS__))
#define LogError(message) \
  (::ykkz000::plugin::Api()->log(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kError), \
                                 (message)))
#define LogErrorF(...)                                                                          \
  (::ykkz000::plugin::Api()->logf(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kError), \
                                  __VA_ARGS__))
#define LogFatal(message) \
  (::ykkz000::plugin::Api()->log(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kFatal), \
                                 (message)))
#define LogFatalF(...)                                                                          \
  (::ykkz000::plugin::Api()->logf(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kFatal), \
                                  __VA_ARGS__))

// -- Member access layer: member reference -> offset; every read is validated via host->readField --

/// @brief Gets a field offset from a member pointer.
/// @tparam TObj Object type.
/// @tparam TField Field type.
/// @param[in] member Member pointer.
/// @return The field's byte offset relative to the object start.
/// @note Takes the member address relative to an aligned static dummy object, to avoid taking the
///       address of a null pointer.
template <class TObj, class TField>
[[nodiscard]] std::size_t MemberOffset(TField TObj::* member) {
  static const TObj kDummy{};
  const auto base = reinterpret_cast<std::uintptr_t>(&kDummy);
  const auto field = reinterpret_cast<std::uintptr_t>(&(kDummy.*member));
  return static_cast<std::size_t>(field - base);
}

/// @brief Reads a member field after validating it via host->readField.
/// @tparam TObj Object type.
/// @tparam TField Field type.
/// @param[in] base Object base address.
/// @param[in] member Member pointer.
/// @param[out] out Receives the read result.
/// @return true on success, otherwise false.
template <class TObj, class TField>
[[nodiscard]] bool TryRead(const void* base, TField TObj::* member, TField& out) {
  const bridge::Host* host = Context().host;
  if (host == nullptr || host->readField == nullptr) {
    return false;
  }
  return host->readField(base, MemberOffset(member), sizeof(TField), &out) != 0;
}

/// @brief Reads a member field after checking readability, returning fallback on failure.
/// @tparam TObj Object type.
/// @tparam TField Field type.
/// @param[in] base Object base address.
/// @param[in] member Member pointer.
/// @param[in] fallback Value returned when the read fails.
/// @return The field value or fallback.
template <class TObj, class TField>
[[nodiscard]] TField TryReadOr(const void* base, TField TObj::* member, TField fallback) {
  TField value = fallback;
  (void)TryRead(base, member, value);
  return value;
}

/// @brief Writes a member field after validating it via host->writeField.
/// @tparam TObj Object type.
/// @tparam TField Field type.
/// @param[in] base Object base address.
/// @param[in] member Member pointer.
/// @param[in] value Value to write.
/// @return true on success, otherwise false.
template <class TObj, class TField>
bool TryWrite(void* base, TField TObj::* member, const TField& value) {
  const bridge::Host* host = Context().host;
  if (host == nullptr || host->writeField == nullptr) {
    return false;
  }
  return host->writeField(base, MemberOffset(member), sizeof(TField), &value) != 0;
}

/// @brief Raw-offset read when there is no member type (validated via host->readField).
/// @tparam T Field type.
/// @param[in] base Base address.
/// @param[in] offset Field offset.
/// @param[out] out Receives the read result.
/// @return true on success, otherwise false.
template <typename T>
[[nodiscard]] bool TryReadAt(const void* base, std::size_t offset, T& out) {
  const bridge::Host* host = Context().host;
  if (host == nullptr || host->readField == nullptr) {
    return false;
  }
  return host->readField(base, offset, sizeof(T), &out) != 0;
}

/// @brief Raw-offset write when there is no member type (validated via host->writeField).
/// @tparam T Field type.
/// @param[in] base Base address.
/// @param[in] offset Field offset.
/// @param[in] value Value to write.
/// @return true on success, otherwise false.
template <typename T>
bool TryWriteAt(void* base, std::size_t offset, const T& value) {
  const bridge::Host* host = Context().host;
  if (host == nullptr || host->writeField == nullptr) {
    return false;
  }
  return host->writeField(base, offset, sizeof(T), &value) != 0;
}

/// @brief Determines whether [address, address+bytes) is readable.
/// @param[in] address Start address.
/// @param[in] bytes Length.
/// @return true when readable.
[[nodiscard]] inline bool IsReadable(const void* address, std::size_t bytes) {
  return Api()->is_readable(address, bytes) != 0;
}

/// @brief Determines whether a pointer looks like a dereferenceable object.
/// @param[in] pointer Pointer to test.
/// @return true when it looks like a candidate object.
[[nodiscard]] inline bool IsCandidateObject(const void* pointer) {
  return Api()->is_candidate_object(pointer) != 0;
}

// -- Player access --
// The engine does not store a "suzerain count" on the player: the suzerain relation lives on each
// city-state's Player::Influence::suzerain (+0x418), and walking the player vector counting those
// whose Influence::suzerain == the target player type yields the suzerain count.

/// @brief Gets the player-vector range.
/// @param[out] begin Vector first pointer.
/// @param[out] end Vector past-the-end pointer.
/// @return true on success.
[[nodiscard]] inline bool GetPlayerVector(civ6::Player::Instance**& begin,
                                          civ6::Player::Instance**& end) {
  void* raw_begin = nullptr;
  void* raw_end = nullptr;
  if (Api()->get_player_vector(&raw_begin, &raw_end) == 0) {
    return false;
  }
  begin = reinterpret_cast<civ6::Player::Instance**>(raw_begin);
  end = reinterpret_cast<civ6::Player::Instance**>(raw_end);
  return true;
}

/// @brief Whether the candidate is a member of the player vector.
/// @param[in] candidate Candidate pointer.
/// @return true when it is a member of the player vector.
[[nodiscard]] inline bool IsRealPlayer(const void* candidate) {
  return Api()->is_real_player(candidate) != 0;
}

/// @brief Exact lookup in the player vector by the player type at +0xD8.
/// @param[in] player_id Player type id.
/// @return The player pointer on a hit, otherwise nullptr.
[[nodiscard]] inline void* PlayerById(int player_id) { return Api()->player_by_id(player_id); }

/// @brief Takes a player directly by index (bounds strictly limited to the player vector).
/// @param[in] index Vector index.
/// @return The player pointer on a hit, otherwise nullptr.
[[nodiscard]] inline void* PlayerAtIndex(int index) { return Api()->player_at_index(index); }

/// @brief Resolves a player type/id into the real player instance.
/// @param[in] player_id Player type/id (Player::Instance +0xD8 value).
/// @return The player pointer on a hit, otherwise nullptr.
[[nodiscard]] inline void* PlayerForOwnerId(int player_id) {
  return Api()->player_for_owner_id(player_id);
}

/// @brief Resolves any object that "carries a player-type field" into a real player.
/// @param[in] object Candidate object.
/// @param[out] via Returns how the match was made, for diagnostics.
/// @return The player pointer on a hit, otherwise nullptr.
[[nodiscard]] inline void* ResolvePlayerFromObject(const void* object, const char*& via) {
  const char* raw_via = "none";
  void* player = Api()->resolve_player_from_object(object, &raw_via);
  via = raw_via;
  return player;
}

/// @brief Counts the number of city-states suzerained by one player.
/// @param[in] player Player pointer.
/// @return The suzerain city count; -1 when any step is untrustworthy.
[[nodiscard]] inline int CountSuzerainsOfPlayer(void* player) {
  return Api()->count_suzerains_of_player(player);
}

} // namespace ykkz000::plugin
