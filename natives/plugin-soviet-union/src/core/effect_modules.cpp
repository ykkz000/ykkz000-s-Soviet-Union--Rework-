#include <ykkz000/plugin/effect_modules.h>

#include <ykkz000/plugin/adjust_city_yield_modifier_per_population.h>
#include <ykkz000/plugin/adjust_city_yield_modifier_per_suzerain.h>
#include <ykkz000/plugin/adjust_player_strength_modifier_per_suzerain.h>

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
