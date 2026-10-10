#include <ykkz000/plugin/object_lifecycle.h>

#include <cstdint>

#include <ykkz000/bridge/host.h>
#include <ykkz000/civ6/city.h>
#include <ykkz000/civ6/player.h>
#include <ykkz000/civ6/unit.h>
#include <ykkz000/extra/player_extra.h>

#include <ykkz000/plugin/engine_access.h>
#include <ykkz000/plugin/persistence_api.h>

// Object lifecycle of this mod's extension side tables.
//
// The side tables (extra::PlayerExtras' city/unit entries) are keyed by the **object pointer**. The
// engine recycles those pointers and the (owner, id) identity they used to carry, so a destroyed city
// (razing/capture) or unit (death/disband) whose records are not dropped would hand stale data to the
// next object that reuses the pointer. The persistence API hooks the engine City/Unit
// constructor/destructor and forwards to these callbacks.
namespace ykkz000::plugin {
namespace {

// Creates the city's extension entry under its owning player (best-effort; identity may not be
// assigned yet at construction, in which case the entry is created later on the first read/write).
void EnsureCityEntry(void* city) {
  if (city == nullptr) {
    return;
  }
  const std::int32_t city_id =
      TryReadOr(city, &civ6::City::Instance::city_id, std::int32_t{-1});
  const std::int32_t owner_id =
      TryReadOr(city, &civ6::City::Instance::owner, std::int32_t{-1});
  if (owner_id < 0 || owner_id > kMaxPlausiblePlayerIndex) {
    return;
  }
  void* const player = PlayerForOwnerId(owner_id);
  if (player == nullptr) {
    return;
  }
  extra::PlayerExtras().EnsureCity(player, city, city_id, owner_id);
}

// Unit side-table identity: unit_id at +0xB0 and owner at +0x128 converted with PlayerTypeIndex. A
// failed or implausible read returns false so the record is left in place rather than erasing the
// wrong entry.
bool UnitKeyOf(const void* unit, std::int32_t& unit_id, int& player_id) {
  unit_id = TryReadOr(unit, &civ6::Unit::Instance::unit_id, std::int32_t{-1});
  if (unit_id < 0) {
    return false;
  }
  const civ6::PlayerTypes owner =
      TryReadOr(unit, &civ6::Unit::Instance::owner, civ6::kInvalidPlayerType);
  player_id = civ6::PlayerTypeIndex(owner);
  if (player_id < 0 || player_id > kMaxPlausiblePlayerIndex) {
    return false;
  }
  return true;
}

// Eagerly create the unit's extension entry (best-effort; the Apply path creates it otherwise).
void EnsureUnitEntry(void* unit) {
  std::int32_t unit_id = -1;
  int player_id = -1;
  if (!UnitKeyOf(unit, unit_id, player_id)) {
    return;
  }
  void* const player = PlayerForOwnerId(player_id);
  if (player == nullptr) {
    return;
  }
  extra::PlayerExtras().EnsureUnit(player, unit, unit_id, player_id);
}

void CityCreated(void* /*user*/, void* city) { EnsureCityEntry(city); }

void CityDestroyed(void* /*user*/, void* city) { extra::PlayerExtras().EraseCity(city); }

void UnitCreated(void* /*user*/, void* unit) { EnsureUnitEntry(unit); }

void UnitDestroyed(void* /*user*/, void* unit) { extra::PlayerExtras().EraseUnit(unit); }

} // namespace

bool RegisterObjectLifecycle() {
  const PersistenceApi* api = Persistence();
  if (api == nullptr) {
    return false;
  }
  return api->register_object_lifecycle(nullptr, &CityCreated, &CityDestroyed, &UnitCreated,
                                        &UnitDestroyed) != 0;
}

} // namespace ykkz000::plugin
