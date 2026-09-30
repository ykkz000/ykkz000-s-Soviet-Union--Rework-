#include "effect_modules.h"

#include "adjust_city_yield_modifier_per_suzerain.h"
#include "adjust_city_yield_per_population_modifier.h"
#include "adjust_player_strength_per_suzerain_modifier.h"

namespace ykkz000::plugin {
namespace {

// The only list that must be maintained when adding an effect: each module assembles itself and
// provides its context/cleanup entry points.
const EffectModule* const kModules[] = {
    CityYieldModule(),
    CityYieldPerSuzerainModule(),
    StrengthModule(),
};

} // namespace

std::span<const EffectModule* const> AllEffectModules() { return kModules; }

} // namespace ykkz000::plugin
