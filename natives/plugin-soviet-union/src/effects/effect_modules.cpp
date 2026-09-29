#include "effect_modules.h"

#include "city_yield_per_population.h"
#include "strength_per_suzerain.h"

namespace ykkz000::plugin {
namespace {

// 唯一需要维护的“新增效果”清单：每个模块自行装配并提供上下文/清理入口。
const EffectModule* const kModules[] = {
    CityYieldModule(),
    StrengthModule(),
};

} // namespace

std::span<const EffectModule* const> AllEffectModules() { return kModules; }

} // namespace ykkz000::plugin
