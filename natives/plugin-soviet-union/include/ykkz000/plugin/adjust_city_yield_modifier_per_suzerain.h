#pragma once

#include <ykkz000/bridge/host.h>

#include <ykkz000/plugin/effect_module.h>

/// @file adjust_city_yield_modifier_per_suzerain.h
/// @brief Effect module for the "city yield percent per suzerain" modifier, backed by
///   EffectType EFFECT_YKKZ000_ADJUST_CITY_YIELD_MODIFIER_PER_SUZERAIN.
/// @note Apply/Remove replace slots with the shared 4-pointer bridge::ApplyFn signature; the
///       city-yield template only consumes the first two arguments (self, city) and ignores the
///       rest. Apply/Remove only maintain the per-suzerain percentage in the CityExtra side table
///       (see extra::CityExtra::per_suzerain_percent); the multiplication by the current suzerain
///       count happens on the engine's yield read path. That injection is owned by the
///       adjust_city_yield_modifier_per_population module's CalculateYield hook, so this module installs no
///       hook of its own (the same target must not be hooked twice, and the hook is the shared
///       host for both modifiers).
namespace ykkz000::plugin {

/// @brief Effect module entry: assembles its own EffectImpl/EffectDesc (prepare = null).
/// @return Resident EffectModule pointer for the module registry to enumerate.
const EffectModule* CityYieldPerSuzerainModule();

} // namespace ykkz000::plugin
