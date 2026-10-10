#include <ykkz000/plugin/effecttype_api.h>

#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>

#include <ykkz000/bridge/host.h>
#include <ykkz000/bridge/log.h>
#include <ykkz000/civ6/city.h>
#include <ykkz000/civ6/game_manager.h>
#include <ykkz000/civ6/player.h>
#include <ykkz000/civ6/unit.h>

// Implementation of the ykkz000_plugin_api_effecttype C ABI. The API plugin is a thin,
// gameplay-agnostic service: it captures the host and exposes validated engine access so consumers
// share one implementation instead of each linking its own copy.
namespace ykkz000::plugin {
namespace {

PluginContext g_context;

constexpr int kMaxPlayerVectorBytes = 256;

// -- Local member-access helpers (validated against g_context.host->readField) --

template <class TObj, class TField>
std::size_t MemberOffset(TField TObj::* member) {
  static const TObj kDummy{};
  const auto base = reinterpret_cast<std::uintptr_t>(&kDummy);
  const auto field = reinterpret_cast<std::uintptr_t>(&(kDummy.*member));
  return static_cast<std::size_t>(field - base);
}

template <class TObj, class TField>
bool TryRead(const void* base, TField TObj::* member, TField& out) {
  const bridge::Host* host = g_context.host;
  if (host == nullptr || host->readField == nullptr) {
    return false;
  }
  return host->readField(base, MemberOffset(member), sizeof(TField), &out) != 0;
}

template <class TObj, class TField>
TField TryReadOr(const void* base, TField TObj::* member, TField fallback) {
  TField value = fallback;
  (void)TryRead(base, member, value);
  return value;
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

// -- Context and registration --

void ApiSetContext(const bridge::Host* host) {
  g_context.host = host;
  g_context.engine = host != nullptr ? host->engine : nullptr;
}

const PluginContext* ApiGetContext() { return &g_context; }

int ApiRegisterEffect(const bridge::EffectDesc* desc) {
  if (desc == nullptr || g_context.host == nullptr ||
      g_context.host->registerEffectType == nullptr) {
    return -1;
  }
  return g_context.host->registerEffectType(desc);
}

int ApiRegisterEffects(const Effect* const* effects, std::uint32_t count) {
  if (effects == nullptr || g_context.host == nullptr) {
    return -1;
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    const Effect* effect = effects[i];
    if (effect == nullptr || effect->describe == nullptr) {
      continue;
    }
    const bridge::EffectDesc* desc = effect->describe(*g_context.host);
    if (desc == nullptr) {
      return -1;
    }
    const int result = ApiRegisterEffect(desc);
    if (result != 0) {
      return result;
    }
  }
  return 0;
}

// -- Memory validation --

int ApiIsReadable(const void* address, std::size_t bytes) {
  return IsReadable(address, bytes) ? 1 : 0;
}

int ApiIsCandidateObject(const void* pointer) { return IsCandidateObject(pointer) ? 1 : 0; }

// -- Player access --

int ApiGetPlayerVector(void** begin, void** end) {
  if (begin == nullptr || end == nullptr) {
    return 0;
  }
  const bridge::EngineApi* engine = g_context.engine;
  if (engine == nullptr || engine->getGameManager == nullptr) {
    return 0;
  }
  using GetGameManagerFn = void* (*)();
  void* manager = reinterpret_cast<GetGameManagerFn>(engine->getGameManager)();
  if (!IsCandidateObject(manager)) {
    return 0;
  }
  civ6::VectorView<civ6::Player::Instance*> players{};
  if (!TryRead(manager, &civ6::GameManager::players, players)) {
    return 0;
  }
  const auto begin_address = reinterpret_cast<std::uintptr_t>(players.begin);
  const auto end_address = reinterpret_cast<std::uintptr_t>(players.end);
  if (!IsCandidateObject(players.begin) || end_address <= begin_address ||
      (end_address - begin_address) > kMaxPlayerVectorBytes * sizeof(void*)) {
    return 0;
  }
  *begin = players.begin;
  *end = players.end;
  return 1;
}

int ApiIsRealPlayer(const void* candidate) {
  civ6::Player::Instance** begin = nullptr;
  civ6::Player::Instance** end = nullptr;
  if (candidate == nullptr || !ApiGetPlayerVector(reinterpret_cast<void**>(&begin),
                                                  reinterpret_cast<void**>(&end))) {
    return 0;
  }
  for (civ6::Player::Instance** it = begin; it != end; ++it) {
    if (!IsReadable(it, sizeof(void*))) {
      return 0;
    }
    if (*it == candidate) {
      return 1;
    }
  }
  return 0;
}

void* ApiPlayerAtIndex(int index) {
  civ6::Player::Instance** begin = nullptr;
  civ6::Player::Instance** end = nullptr;
  if (index < 0 || !ApiGetPlayerVector(reinterpret_cast<void**>(&begin),
                                       reinterpret_cast<void**>(&end))) {
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

void* ApiPlayerById(int player_id) {
  civ6::Player::Instance** begin = nullptr;
  civ6::Player::Instance** end = nullptr;
  if (player_id < 0 || !ApiGetPlayerVector(reinterpret_cast<void**>(&begin),
                                           reinterpret_cast<void**>(&end))) {
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

void* ApiPlayerForOwnerId(int player_id) {
  if (player_id < 0 || player_id > kMaxPlausiblePlayerIndex) {
    return nullptr;
  }
  // Prefer matching by +0xD8 (index != player type); on failure fall back to taking the real player
  // at that index.
  void* player = ApiPlayerById(player_id);
  if (player != nullptr && ApiIsRealPlayer(player) != 0) {
    return player;
  }
  player = ApiPlayerAtIndex(player_id);
  return player != nullptr && ApiIsRealPlayer(player) != 0 ? player : nullptr;
}

void* ApiResolvePlayerFromObject(const void* object, const char** via) {
  if (via != nullptr) {
    *via = "none";
  }
  if (object == nullptr) {
    return nullptr;
  }
  if (ApiIsRealPlayer(object) != 0) {
    if (via != nullptr) {
      *via = "ptr";
    }
    return const_cast<void*>(object);
  }
  const civ6::PlayerTypes type =
      TryReadOr(object, &civ6::Player::Instance::player_type, civ6::kInvalidPlayerType);
  const int player_id = civ6::PlayerTypeIndex(type);
  if (player_id < 0 || player_id > kMaxPlausiblePlayerIndex) {
    return nullptr;
  }
  if (void* player = ApiPlayerById(player_id); player != nullptr) {
    if (via != nullptr) {
      *via = "id";
    }
    return player;
  }
  void* player = ApiPlayerAtIndex(player_id);
  if (player != nullptr && ApiIsRealPlayer(player) != 0) {
    if (via != nullptr) {
      *via = "idx";
    }
    return player;
  }
  return nullptr;
}

int ApiCountSuzerainsOfPlayer(void* player) {
  if (ApiIsRealPlayer(player) == 0) {
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
  if (!ApiGetPlayerVector(reinterpret_cast<void**>(&begin), reinterpret_cast<void**>(&end))) {
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

const EffectTypeApi kApiTable = {
    kEffectTypeApiVersion,       // version
    &ApiSetContext,              // set_context
    &ApiGetContext,              // get_context
    &ApiRegisterEffect,          // register_effect
    &ApiRegisterEffects,         // register_effects
    &Log,                        // log
    &LogF,                       // logf
    &ApiIsReadable,              // is_readable
    &ApiIsCandidateObject,       // is_candidate_object
    &ApiGetPlayerVector,         // get_player_vector
    &ApiIsRealPlayer,            // is_real_player
    &ApiPlayerById,              // player_by_id
    &ApiPlayerAtIndex,           // player_at_index
    &ApiPlayerForOwnerId,        // player_for_owner_id
    &ApiResolvePlayerFromObject, // resolve_player_from_object
    &ApiCountSuzerainsOfPlayer,  // count_suzerains_of_player
};

} // namespace

} // namespace ykkz000::plugin

extern "C" const ykkz000::plugin::EffectTypeApi* GetEffectTypeApi(std::uint32_t version) {
  if (version > ykkz000::plugin::kEffectTypeApiVersion) {
    return nullptr;
  }
  return &ykkz000::plugin::kApiTable;
}
