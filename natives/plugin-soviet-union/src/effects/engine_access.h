#pragma once

#include <cstddef>
#include <cstdint>

#include <ykkz000/bridge/host.h>
#include <ykkz000/bridge/log.h>
#include <ykkz000/civ6/city.h>
#include <ykkz000/civ6/game_manager.h>
#include <ykkz000/civ6/player.h>
#include <ykkz000/civ6/unit.h>

/// @file engine_access.h
/// @brief Plugin-side engine access layer.
/// @note Wraps the host services (validated reads/writes, player/city resolution) into type-safe
///       accessors. The Loader no longer recognizes any concrete behavior, and every engine read
///       must go through validation such as host->readField. Pointers handed out by the engine are
///       always treated as untrusted: run a readability/candidate check first, skip on failure, and
///       never write to an unvalidated address.
namespace ykkz000::plugin {

/// @brief Runtime defensive bounds.
/// @note A sudden jump in entry count or an absurd population/player index is usually a symptom of
///       an invalid self/city pointer; in that case it is better to skip the write than to corrupt a
///       garbage address.
inline constexpr int kMaxEffectEntries = 64;
inline constexpr int kMaxPlausiblePopulation = 100000;
inline constexpr int kMaxPlausiblePlayerIndex = 255;
inline constexpr int kMaxPlausibleSuzerainCount = 128;

/// @brief Plugin global context: host services and engine entry points (set in GetPlugin).
struct PluginContext {
  const bridge::Host* host = nullptr;
  const bridge::EngineApi* engine = nullptr;
};

/// @brief Sets the plugin global context.
/// @param[in] host Host service table.
void SetContext(const bridge::Host* host);
/// @brief Gets the plugin global context.
/// @return The PluginContext reference.
[[nodiscard]] const PluginContext& Context();

/// @brief Emits one log line.
/// @param[in] level Log level.
/// @param[in] message Text.
void Log(int level, const char* message);
/// @brief Formats and emits one log line (printf style).
/// @param[in] level Log level.
/// @param[in] format Format string.
/// @param[in] ... Format arguments.
void LogF(int level, const char* format, ...);

// -- Level wrappers --
// Zero-overhead level macros mirroring the Loader's. TRACE is never used; DEBUG is compiled out
// entirely unless _DEBUG is defined (so its formatting arguments are not evaluated). Plugin
// logs are forwarded through host->log, where the Loader applies the runtime filter.
#define LogTrace(message) ((void)0)
#define LogTraceF(...) ((void)0)
#if defined(_DEBUG)
#define LogDebug(message) \
  Log(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kDebug), (message))
#define LogDebugF(...) \
  LogF(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kDebug), __VA_ARGS__)
#else
#define LogDebug(message) ((void)0)
#define LogDebugF(...) ((void)0)
#endif
#define LogInfo(message) \
  Log(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kInfo), (message))
#define LogInfoF(...) \
  LogF(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kInfo), __VA_ARGS__)
#define LogWarn(message) \
  Log(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kWarning), (message))
#define LogWarnF(...) \
  LogF(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kWarning), __VA_ARGS__)
#define LogError(message) \
  Log(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kError), (message))
#define LogErrorF(...) \
  LogF(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kError), __VA_ARGS__)
#define LogFatal(message) \
  Log(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kFatal), (message))
#define LogFatalF(...) \
  LogF(::ykkz000::bridge::ToInt(::ykkz000::bridge::LogLevel::kFatal), __VA_ARGS__)

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
[[nodiscard]] bool IsReadable(const void* address, std::size_t bytes);
/// @brief Determines whether a pointer looks like a dereferenceable object.
/// @param[in] pointer Pointer to test.
/// @return true when it looks like a candidate object.
[[nodiscard]] bool IsCandidateObject(const void* pointer);

// -- Player access --
// The engine does not store a "suzerain count" on the player: the suzerain relation lives on each
// city-state's Player::Influence::suzerain (+0x418), and walking the player vector counting those
// whose Influence::suzerain == the target player type yields the suzerain count.

/// @brief Gets the player-vector range.
/// @param[out] begin Vector first pointer.
/// @param[out] end Vector past-the-end pointer.
/// @return true on success.
[[nodiscard]] bool GetPlayerVector(civ6::Player::Instance**& begin,
                                   civ6::Player::Instance**& end);
/// @brief Whether the candidate is a member of the player vector (exact match, ruling out the
///   "readable therefore valid" false positive).
/// @param[in] candidate Candidate pointer.
/// @return true when it is a member of the player vector.
[[nodiscard]] bool IsRealPlayer(const void* candidate);
/// @brief Exact lookup in the player vector by the player type at +0xD8.
/// @param[in] player_id Player type id.
/// @return The player pointer on a hit, otherwise nullptr.
/// @note The index is not guaranteed to equal the player type.
[[nodiscard]] void* PlayerById(int player_id);
/// @brief Takes a player directly by index (bounds strictly limited to the player vector).
/// @param[in] index Vector index.
/// @return The player pointer on a hit, otherwise nullptr.
[[nodiscard]] void* PlayerAtIndex(int index);
/// @brief Resolves any object that "carries a player-type field" into a real player.
/// @param[in] object Candidate object.
/// @param[out] via Returns how the match was made, for diagnostics.
/// @return The player pointer on a hit, otherwise nullptr.
[[nodiscard]] void* ResolvePlayerFromObject(const void* object, const char*& via);
/// @brief Counts the number of city-states suzerained by one player.
/// @param[in] player Player pointer.
/// @return The suzerain city count; -1 when any step is untrustworthy.
[[nodiscard]] int CountSuzerainsOfPlayer(void* player);

} // namespace ykkz000::plugin
