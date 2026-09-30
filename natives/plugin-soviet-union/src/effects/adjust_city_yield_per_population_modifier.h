#pragma once

#include <ykkz000/bridge/host.h>

#include "effect_module.h"

/// @file adjust_city_yield_per_population_modifier.h
/// @brief Effect module for the "city yield per population percent" modifier, backed by
///   EffectType EFFECT_YKKZ000_ADJUST_CITY_YIELD_PER_POPULATION_MODIFIER.
/// @note Apply/Remove replace slots with the shared 4-pointer bridge::ApplyFn signature; the
///       city-yield template only consumes the first two arguments (self, city) and ignores the
///       rest. Apply/Remove only maintain the per-citizen percentage in the CityExtra side table
///       (see extra::CityExtra::percent); the multiplication by population happens on the engine's
///       yield read path.
namespace ykkz000::plugin {

/// @brief Effect module entry: assembles its own EffectImpl/EffectDesc (including the hook
///   install entry point).
/// @return Resident EffectModule pointer for the module registry to enumerate.
const EffectModule* CityYieldModule();

} // namespace ykkz000::plugin
