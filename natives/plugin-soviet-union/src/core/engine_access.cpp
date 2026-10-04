#include <ykkz000/plugin/engine_access.h>

#include <cstdarg>
#include <cstdio>

namespace ykkz000::plugin {
namespace {

PluginContext g_context;

constexpr int kMaxPlayerVectorBytes = 256;

} // namespace

void SetContext(const bridge::Host* host) {
  g_context.host = host;
  g_context.engine = host != nullptr ? host->engine : nullptr;
}

const PluginContext& Context() {
  return g_context;
}

void Log(int level, const char* message) {
  if (g_context.host != nullptr && g_context.host->log != nullptr) {
    g_context.host->log(level, message);
  }
}

void LogF(int level, const char* format, ...) {
  char buffer[1024] = {};
  va_list args;
  va_start(args, format);
  _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
  va_end(args);
  Log(level, buffer);
}

bool IsReadable(const void* address, std::size_t bytes) {
  const bridge::Host* host = g_context.host;
  return host != nullptr && host->isReadableRegion != nullptr &&
         host->isReadableRegion(address, bytes) != 0;
}

bool IsCandidateObject(const void* pointer) {
  const bridge::Host* host = g_context.host;
  return host != nullptr && host->isCandidateObject != nullptr &&
         host->isCandidateObject(pointer) != 0;
}

bool GetPlayerVector(civ6::Player::Instance**& begin, civ6::Player::Instance**& end) {
  const bridge::EngineApi* engine = g_context.engine;
  if (engine == nullptr || engine->getGameManager == nullptr) {
    return false;
  }
  using GetGameManagerFn = void* (*)();
  void* manager = reinterpret_cast<GetGameManagerFn>(engine->getGameManager)();
  if (!IsCandidateObject(manager)) {
    return false;
  }
  civ6::VectorView<civ6::Player::Instance*> players{};
  if (!TryRead(manager, &civ6::GameManager::players, players)) {
    return false;
  }
  begin = players.begin;
  end = players.end;
  const auto beginAddress = reinterpret_cast<std::uintptr_t>(begin);
  const auto endAddress = reinterpret_cast<std::uintptr_t>(end);
  if (!IsCandidateObject(begin) || endAddress <= beginAddress ||
      (endAddress - beginAddress) > kMaxPlayerVectorBytes * sizeof(void*)) {
    return false;
  }
  return true;
}

bool IsRealPlayer(const void* candidate) {
  civ6::Player::Instance** begin = nullptr;
  civ6::Player::Instance** end = nullptr;
  if (candidate == nullptr || !GetPlayerVector(begin, end)) {
    return false;
  }
  for (civ6::Player::Instance** it = begin; it != end; ++it) {
    if (!IsReadable(it, sizeof(void*))) {
      return false;
    }
    if (*it == candidate) {
      return true;
    }
  }
  return false;
}

void* PlayerAtIndex(int index) {
  civ6::Player::Instance** begin = nullptr;
  civ6::Player::Instance** end = nullptr;
  if (index < 0 || !GetPlayerVector(begin, end)) {
    return nullptr;
  }
  const std::size_t count =
      (reinterpret_cast<std::uintptr_t>(end) - reinterpret_cast<std::uintptr_t>(begin)) /
      sizeof(void*);
  if (static_cast<std::size_t>(index) >= count) {
    return nullptr;
  }
  return begin[index];
}

void* PlayerById(int player_id) {
  civ6::Player::Instance** begin = nullptr;
  civ6::Player::Instance** end = nullptr;
  if (player_id < 0 || !GetPlayerVector(begin, end)) {
    return nullptr;
  }
  for (civ6::Player::Instance** it = begin; it != end; ++it) {
    if (!IsReadable(it, sizeof(void*))) {
      return nullptr;
    }
    civ6::Player::Instance* player = *it;
    if (player == nullptr) {
      continue;
    }
    const civ6::PlayerTypes type =
        TryReadOr(player, &civ6::Player::Instance::player_type, civ6::kInvalidPlayerType);
    if (civ6::PlayerTypeIndex(type) == player_id) {
      return player;
    }
  }
  return nullptr;
}

void* ResolvePlayerFromObject(const void* object, const char*& via) {
  via = "none";
  if (object == nullptr) {
    return nullptr;
  }
  if (IsRealPlayer(object)) {
    via = "ptr";
    return const_cast<void*>(object);
  }
  const civ6::PlayerTypes type =
      TryReadOr(object, &civ6::Player::Instance::player_type, civ6::kInvalidPlayerType);
  const int player_id = civ6::PlayerTypeIndex(type);
  if (player_id < 0 || player_id > kMaxPlausiblePlayerIndex) {
    return nullptr;
  }
  if (void* player = PlayerById(player_id); player != nullptr) {
    via = "id";
    return player;
  }
  void* player = PlayerAtIndex(player_id);
  if (player != nullptr && IsRealPlayer(player)) {
    via = "idx";
    return player;
  }
  return nullptr;
}

int CountSuzerainsOfPlayer(void* player) {
  if (!IsRealPlayer(player)) {
    return -1; // Must be a real player from the player vector, not a merely "readable" fake pointer
  }
  const civ6::PlayerTypes target =
      TryReadOr(player, &civ6::Player::Instance::player_type, civ6::kInvalidPlayerType);
  const int target_index = civ6::PlayerTypeIndex(target);
  if (target_index < 0 || target_index > kMaxPlausiblePlayerIndex) {
    return -1;
  }
  civ6::Player::Instance** begin = nullptr;
  civ6::Player::Instance** end = nullptr;
  if (!GetPlayerVector(begin, end)) {
    return -1;
  }
  int count = 0;
  for (civ6::Player::Instance** it = begin; it != end; ++it) {
    if (!IsReadable(it, sizeof(void*))) {
      return -1;
    }
    void* candidate = *it;
    if (candidate == nullptr) {
      continue;
    }
    civ6::Player::Influence* influence = nullptr;
    if (!TryRead(candidate, &civ6::Player::Instance::influence, influence) ||
        !IsCandidateObject(influence)) {
      continue; // No Influence or untrusted pointer: this player contributes no suzerain count
    }
    const civ6::PlayerTypes suzerain =
        TryReadOr(influence, &civ6::Player::Influence::suzerain, civ6::kInvalidPlayerType);
    if (suzerain == target) {
      ++count;
    }
  }
  if (count > kMaxPlausibleSuzerainCount) {
    return -1;
  }
  return count;
}

} // namespace ykkz000::plugin
