#pragma once

#include <ykkz000/bridge/host.h>

#include "effect_module.h"

/// @file adjust_player_strength_per_suzerain_modifier.h
/// @brief Effect module for the "player unit strength per suzerain" modifier, backed by
///   EffectType EFFECT_YKKZ000_ADJUST_PLAYER_STRENGTH_PER_SUZERAIN_MODIFIER.
/// @note Apply/Remove replace slots with the shared 4-pointer bridge::ApplyFn signature (identical
///       to the real signature of the strength template).
namespace ykkz000::plugin {

/// @brief Effect module entry: assembles its own EffectImpl/EffectDesc (including the hook
///   install entry point).
/// @return Resident EffectModule pointer for the module registry to enumerate.
const EffectModule* StrengthModule();

} // namespace ykkz000::plugin
