#include <ykkz000/plugin/effects.h>

#include <cstdint>
#include <limits>
#include <vector>

#include <ykkz000/bridge/host.h>
#include <ykkz000/civ6/effect.h>
#include <ykkz000/extra/player_extra.h>

#include <ykkz000/plugin/city_yield_common.h>
#include <ykkz000/plugin/engine_access.h>
#include <ykkz000/plugin/extra_persistence.h>

// City-yield modifier of "per suzerain city x Amount%" (multiply by suzerain count at read time).
//
// Shares the same side table (CityExtra) and the same injection point as
// adjust_city_yield_modifier_per_population: Apply/Remove only aggregate the per-suzerain percentage into
// per_suzerain_percent[] (FixedPoint<16>, 0x10000 == +1%/suzerain); the actual
// (value x current suzerain count) >> 8 conversion happens inside the CalculateYield hook owned by
// the adjust_city_yield_modifier_per_population module, so suzerain changes are followed naturally by the
// next read and no bookkeeping is needed here.
//
// This module installs no hook (prepare = null): the CalculateYield MinHook is owned exclusively by
// the existing module (the same target must not be hooked twice); side-table cleanup is handled
// centrally by context events (see adjust_city_yield_modifier_per_population). If the existing module is
// disabled by DISABLE_CUSTOM_BEHAVIOR, this effect is not injected either (it depends on
// the host).
namespace ykkz000::plugin {
namespace {

// Aggregate entries into the side table's per_suzerain_percent[] by sign(+-1):
//   per_suzerain_percent += sign * Amount * kPercentUnit.
void ApplyPerSuzerainEntries(void* self, void* city, int sign) {
  if (self == nullptr || city == nullptr) {
    return;
  }
  std::vector<EffectEntry> entries;
  if (!ReadEffectEntries(self, entries)) {
    return;
  }
  CityRef ref;
  if (!CityRefOf(city, ref)) {
    return;
  }
  void* const player = PlayerForOwnerId(ref.player_id);
  if (player == nullptr) {
    return;
  }
  constexpr std::int64_t kPercentMin = std::numeric_limits<std::int32_t>::min();
  constexpr std::int64_t kPercentMax = std::numeric_limits<std::int32_t>::max();
  // Seed from the persisted AutoVariables after a load before the incremental add (see the
  // per-population module for the rationale).
  EnsureCityExtraHydrated(city, ref);
  extra::PlayerExtras().EditCity(
      player, city, ref.city_id, ref.player_id, [&](extra::CityExtra& extra) {
        for (const EffectEntry& entry : entries) {
          if (entry.yield_type < 0 ||
              entry.yield_type >= static_cast<int>(civ6::kMaxYields)) {
            continue;
          }
          if (entry.amount < -kMaxPlausibleAmount || entry.amount > kMaxPlausibleAmount) {
            continue;
          }
          const std::int64_t delta =
              static_cast<std::int64_t>(entry.amount) * kPercentUnit * sign;
          std::int64_t updated = static_cast<std::int64_t>(
                                     extra.per_suzerain_percent[entry.yield_type]) +
                                 delta;
          if (updated > kPercentMax) {
            updated = kPercentMax;
          } else if (updated < kPercentMin) {
            updated = kPercentMin;
          }
          extra.per_suzerain_percent[entry.yield_type] =
              static_cast<std::int32_t>(updated);
          if (entry.yield_type + 1 > extra.yield_count) {
            extra.yield_count = entry.yield_type + 1;
          }
        }
        PersistCityValues(city, extra.percent.data(), extra.per_suzerain_percent.data());
      });
  // Same as the existing module: after the side table changes, clear the engine's city-yield cache
  // and send a zero-delta "yield changed" notification, and invalidate the TLS snapshot (EditCity
  // already wrote, so the read path must see the new value).
  InvalidateAndNotifyCityYield(city, entries);
  // The entry is never erased on a zeroing Remove: extensions exist for every live city.
  InvalidateCityExtraSnapshotCache();
}

// Apply/Remove slots (bridge::ApplyFn signature).
std::uint64_t Apply(void* self, void* a1, void* a2, void* a3) {
  (void)a2;
  (void)a3;
  ApplyPerSuzerainEntries(self, a1, 1);
  return static_cast<std::uint64_t>(1);
}

std::uint64_t Remove(void* self, void* a1, void* a2, void* a3) {
  (void)a2;
  (void)a3;
  ApplyPerSuzerainEntries(self, a1, -1);
  return static_cast<std::uint64_t>(1);
}

// Resident assembly objects: the loader keeps the impl pointer, so they must not be temporaries.
bridge::EffectImpl g_impl = {};
bridge::EffectDesc g_desc = {};
bool g_described = false;

// Assemble this module's EffectImpl/EffectDesc; returns the resident desc for plugin.cpp to
// register.
const bridge::EffectDesc* Describe(const bridge::Host& host) {
  (void)host;
  if (g_described) {
    return &g_desc;
  }
  g_desc.typeName = "EFFECT_YKKZ000_ADJUST_CITY_YIELD_MODIFIER_PER_SUZERAIN";
  g_desc.templateEffect = "EFFECT_ADJUST_CITY_YIELD_MODIFIER";
#if !defined(DISABLE_CUSTOM_BEHAVIOR)
  const bridge::EngineApi* engine = host.engine;
  if (engine == nullptr) {
    return nullptr;
  }
  g_impl.templateApply = engine->effectApply;
  g_impl.templateRemove = engine->effectRemove;
  g_impl.apply = &Apply;
  g_impl.remove = &Remove;
  g_impl.label = "city-yield-per-suzerain";
  g_desc.impl = &g_impl;
  // No hook: the CalculateYield injection is owned exclusively by the existing city-yield module.
  g_desc.prepare = nullptr;
#else
  g_desc.impl = nullptr;    // Custom behavior disabled: degrade to the template behavior
  g_desc.prepare = nullptr; // No hook
#endif
  g_described = true;
  return &g_desc;
}

const Effect g_effect = {"city-yield-per-suzerain", &Describe, nullptr, nullptr};

} // namespace

const Effect* GetAdjustCityYieldModifierPerSuzerainEffect() { return &g_effect; }

} // namespace ykkz000::plugin
