#include <ykkz000/plugin/object_cleanup.h>

#include <atomic>
#include <cstdint>
#include <mutex>

#include <ykkz000/bridge/host.h>
#include <ykkz000/civ6/unit.h>
#include <ykkz000/extra/player_extra.h>

#include <ykkz000/plugin/city_yield_common.h>
#include <ykkz000/plugin/engine_access.h>
#include <ykkz000/plugin/extra_persistence.h>

// Object lifecycle of this mod's extension side tables.
//
// The side tables (extra::PlayerExtras' city/unit entries and the persistence layer's g_cityVars) are
// keyed by the **object pointer**. The engine recycles those pointers and the (owner, id) identity
// they used to carry, so a destroyed city (razing/capture) or unit (death/disband) whose records are
// not dropped would hand stale data to the next object that reuses the pointer. Hooking the City/Unit
// destructors closes that path.
//
// The Loader publishes the plain destructor entry points (GameCore RVAs 0x43940 and 0x39D6A0); the
// engine's deleting destructors call these, so hooking the plain destructor also covers deletion.
// The detours forward to the original destructor first, then erase plugin-owned records by object
// pointer -- they never call any engine function on the half-destroyed object.
//
// The Unit constructor is additionally observed (best-effort) to create the unit's extension entry as
// soon as the object exists, mirroring "an extension for every live object". This hook is optional: if
// it cannot be installed the strength module's Apply path still creates the entry on first use.
namespace ykkz000::plugin {
namespace {

using ObjectDestructorFn = void (*)(void* object);
using ObjectConstructorFn = void* (*)(void* object);

std::mutex g_mutex;
ObjectDestructorFn g_cityDestructorOriginal = nullptr;
ObjectDestructorFn g_unitDestructorOriginal = nullptr;
ObjectConstructorFn g_unitConstructorOriginal = nullptr;
void* g_cityDestructorTarget = nullptr;
void* g_unitDestructorTarget = nullptr;
void* g_unitConstructorTarget = nullptr;
std::atomic<bool> g_enabled{false};

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

// City destructor detour: the object's identity is captured before the original tears the archive
// down; the cleanup afterwards only erases plugin-owned records (keyed by the city pointer).
void CityDestructor_Hook(void* city) {
  CityRef ref;
  const bool captured =
      city != nullptr && g_enabled.load(std::memory_order_acquire) && CityRefOf(city, ref);
  if (g_cityDestructorOriginal != nullptr) {
    g_cityDestructorOriginal(city);
  }
  if (!captured) {
    return;
  }
  ForgetCityVars(city);
  extra::PlayerExtras().EraseCity(city);
}

// Unit destructor detour: validate the object is a plausible unit, forward, then erase the unit's
// side-table entry by pointer.
void UnitDestructor_Hook(void* unit) {
  std::int32_t unit_id = -1;
  int player_id = -1;
  const bool captured = unit != nullptr && g_enabled.load(std::memory_order_acquire) &&
                        UnitKeyOf(unit, unit_id, player_id);
  if (g_unitDestructorOriginal != nullptr) {
    g_unitDestructorOriginal(unit);
  }
  if (!captured) {
    return;
  }
  extra::PlayerExtras().EraseUnit(unit);
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

// Unit constructor detour: forwards, then creates the extension entry when the identity is already
// readable.
void* UnitConstructor_Hook(void* self) {
  void* result = self;
  if (g_unitConstructorOriginal != nullptr) {
    result = g_unitConstructorOriginal(self);
  }
  if (self != nullptr && g_enabled.load(std::memory_order_acquire)) {
    EnsureUnitEntry(self);
  }
  return result;
}

} // namespace

bool EnsureObjectCleanupHooks() {
  const bridge::Host* host = Context().host;
  const bridge::EngineApi* engine = Context().engine;
  if (host == nullptr || host->installHook == nullptr || host->removeHook == nullptr ||
      engine == nullptr || engine->cityDestructor == nullptr ||
      engine->unitDestructor == nullptr) {
    static std::atomic<bool> kLoggedUnavailable{false};
    if (!kLoggedUnavailable.exchange(true)) {
      LogWarn("cleanup: City/Unit destructor entries unavailable; destroyed objects keep their "
              "side-table records until the next context switch");
    }
    return false;
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  // Idempotent fast path: both hooks already installed with usable trampolines.
  if (g_enabled.load(std::memory_order_acquire) && g_cityDestructorOriginal != nullptr &&
      g_unitDestructorOriginal != nullptr) {
    return true;
  }

  // All-or-nothing: a partial install would leave one object kind uncleaned; on any failure remove
  // whatever was installed so the layer stays consistent.
  void* city_original = nullptr;
  int status = host->installHook(host->pluginHandle, engine->cityDestructor,
                                 reinterpret_cast<void*>(&CityDestructor_Hook), &city_original);
  if (status != 0 || city_original == nullptr) {
    (void)host->removeHook(host->pluginHandle, engine->cityDestructor);
    g_cityDestructorTarget = nullptr;
    g_cityDestructorOriginal = nullptr;
    LogErrorF("cleanup: City destructor hook install -> %d", status);
    return false;
  }
  g_cityDestructorTarget = engine->cityDestructor;
  g_cityDestructorOriginal = reinterpret_cast<ObjectDestructorFn>(city_original);

  void* unit_original = nullptr;
  status = host->installHook(host->pluginHandle, engine->unitDestructor,
                             reinterpret_cast<void*>(&UnitDestructor_Hook), &unit_original);
  if (status != 0 || unit_original == nullptr) {
    (void)host->removeHook(host->pluginHandle, engine->unitDestructor);
    (void)host->removeHook(host->pluginHandle, g_cityDestructorTarget);
    g_cityDestructorTarget = nullptr;
    g_cityDestructorOriginal = nullptr;
    g_unitDestructorTarget = nullptr;
    g_unitDestructorOriginal = nullptr;
    LogErrorF("cleanup: Unit destructor hook install -> %d", status);
    return false;
  }
  g_unitDestructorTarget = engine->unitDestructor;
  g_unitDestructorOriginal = reinterpret_cast<ObjectDestructorFn>(unit_original);

  g_enabled.store(true, std::memory_order_release);

  // Optional (non-fatal): eager unit-entry creation on unit construction.
  if (engine->unitConstructor != nullptr) {
    void* ctor_original = nullptr;
    const int ctor_status =
        host->installHook(host->pluginHandle, engine->unitConstructor,
                          reinterpret_cast<void*>(&UnitConstructor_Hook), &ctor_original);
    if (ctor_status == 0 && ctor_original != nullptr) {
      g_unitConstructorTarget = engine->unitConstructor;
      g_unitConstructorOriginal = reinterpret_cast<ObjectConstructorFn>(ctor_original);
    } else {
      LogWarnF("cleanup: Unit constructor hook install -> %d (non-fatal; entries are created on "
               "first Apply)",
               ctor_status);
    }
  }

  LogInfoF("cleanup: object lifecycle hooks active (city-dtor=%p unit-dtor=%p unit-ctor=%p)",
           g_cityDestructorTarget, g_unitDestructorTarget, g_unitConstructorTarget);
  return true;
}

void RemoveObjectCleanupHooks() {
  const bridge::Host* host = Context().host;
  std::lock_guard<std::mutex> lock(g_mutex);
  // Disable the detours before removing them so a concurrent destructor that has already entered the
  // detour cannot erase a record after the context cleanup completes.
  g_enabled.store(false, std::memory_order_release);
  if (host != nullptr && host->removeHook != nullptr) {
    if (g_cityDestructorTarget != nullptr) {
      (void)host->removeHook(host->pluginHandle, g_cityDestructorTarget);
    }
    if (g_unitDestructorTarget != nullptr) {
      (void)host->removeHook(host->pluginHandle, g_unitDestructorTarget);
    }
    if (g_unitConstructorTarget != nullptr) {
      (void)host->removeHook(host->pluginHandle, g_unitConstructorTarget);
    }
  }
  g_cityDestructorTarget = nullptr;
  g_cityDestructorOriginal = nullptr;
  g_unitDestructorTarget = nullptr;
  g_unitDestructorOriginal = nullptr;
  g_unitConstructorTarget = nullptr;
  g_unitConstructorOriginal = nullptr;
}

} // namespace ykkz000::plugin
