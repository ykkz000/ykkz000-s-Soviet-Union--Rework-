#pragma once

#include <ykkz000/bridge/host.h>

#include <ykkz000/plugin/effect_module.h>

/// @file adjust_city_yield_modifier_per_population.h
/// @brief Effect module for the "city yield per population percent" modifier, backed by
///   EffectType EFFECT_YKKZ000_ADJUST_CITY_YIELD_MODIFIER_PER_POPULATION.
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
