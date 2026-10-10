#pragma once

#include <span>

#include <ykkz000/bridge/host.h>
#include <ykkz000/plugin/effect.h>

/// @file effects.h
/// @brief Plugin-side manifest of all custom effects.
/// @note The shared Effect interface lives in the effecttype API plugin (<ykkz000/plugin/effect.h>).
///       Each effect's implementation lives in its own .cpp under src/effects (assembling its own
///       resident EffectImpl/EffectDesc); plugin.cpp only iterates the manifest and knows no concrete
///       effect's name, fields, or toggles.
/// @note Adding an effect = create src/effects/<name>.cpp, declare its Get...Effect entry here, and
///       add one line to GetAllEffects; plugin.cpp needs no changes.
namespace ykkz000::plugin {

/// @brief EffectType EFFECT_YKKZ000_ADJUST_CITY_YIELD_MODIFIER_PER_POPULATION entry.
/// @return Resident Effect pointer for the manifest to enumerate.
/// @note Apply/Remove replace slots with the shared 4-pointer bridge::ApplyFn signature; the
///       city-yield template only consumes the first two arguments (self, city) and ignores the
///       rest. Apply/Remove only maintain the per-citizen percentage in the CityExtra side table
///       (see extra::CityExtra::percent); the multiplication by population happens on the engine's
///       yield read path.
const Effect* GetAdjustCityYieldModifierPerPopulationEffect();

/// @brief EffectType EFFECT_YKKZ000_ADJUST_CITY_YIELD_MODIFIER_PER_SUZERAIN entry.
/// @return Resident Effect pointer for the manifest to enumerate.
/// @note Apply/Remove replace slots with the shared 4-pointer bridge::ApplyFn signature; the
///       city-yield template only consumes the first two arguments (self, city) and ignores the
///       rest. Apply/Remove only maintain the per-suzerain percentage in the CityExtra side table
///       (see extra::CityExtra::per_suzerain_percent); the multiplication by the current suzerain
///       count happens on the engine's yield read path. That injection is owned by the
///       adjust_city_yield_modifier_per_population effect's CalculateYield hook, so this effect
///       installs no hook of its own (the same target must not be hooked twice, and the hook is
///       the shared host for both modifiers).
const Effect* GetAdjustCityYieldModifierPerSuzerainEffect();

/// @brief EffectType EFFECT_YKKZ000_ADJUST_PLAYER_STRENGTH_MODIFIER_PER_SUZERAIN entry.
/// @return Resident Effect pointer for the manifest to enumerate.
/// @note Apply/Remove replace slots with the shared 4-pointer bridge::ApplyFn signature (identical
///       to the real signature of the strength template).
const Effect* GetAdjustPlayerStrengthModifierPerSuzerainEffect();

/// @brief Get all custom effects (manifest).
/// @return Read-only view of every custom effect.
inline std::span<const Effect* const> GetAllEffects() {
  static const Effect* const kEffects[] = {
      GetAdjustCityYieldModifierPerPopulationEffect(),
      GetAdjustCityYieldModifierPerSuzerainEffect(),
      GetAdjustPlayerStrengthModifierPerSuzerainEffect(),
  };
  return kEffects;
}

} // namespace ykkz000::plugin
