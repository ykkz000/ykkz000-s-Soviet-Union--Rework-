#pragma once

#include <ykkz000/bridge/host.h>

#include <ykkz000/plugin/effect_module.h>

/// @file adjust_player_strength_modifier_per_suzerain.h
/// @brief Effect module for the "player unit strength per suzerain" modifier, backed by
///   EffectType EFFECT_YKKZ000_ADJUST_PLAYER_STRENGTH_MODIFIER_PER_SUZERAIN.
/// @note Apply/Remove replace slots with the shared 4-pointer bridge::ApplyFn signature (identical
///       to the real signature of the strength template).
namespace ykkz000::plugin {

/// @brief Effect module entry: assembles its own EffectImpl/EffectDesc (including the hook
///   install entry point).
/// @return Resident EffectModule pointer for the module registry to enumerate.
const EffectModule* StrengthModule();

} // namespace ykkz000::plugin
