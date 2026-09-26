#include <ykkz000/bridge.h>
#include <ykkz000/export.h>

using namespace ykkz000::bridge;

namespace {

int registerEffects(Host* host) {
  EffectDesc fiveYearPlan = {};
  fiveYearPlan.typeName = "EFFECT_YKKZ000_SOVIET_FIVE_YEAR_PLAN";
  fiveYearPlan.templateEffect = "EFFECT_ADJUST_ALL_DISTRICTS_PRODUCTION";
  fiveYearPlan.commonName = nullptr;
  fiveYearPlan.description = nullptr;
  fiveYearPlan.tags = nullptr;
  fiveYearPlan.gameCapabilities = nullptr;
  fiveYearPlan.contextInterfaces = nullptr;
  fiveYearPlan.subjectInterfaces = nullptr;
  fiveYearPlan.supportsRemove = -1;
  fiveYearPlan.behavior = EffectBehavior::kInherit;
  if (host->registerEffectType(&fiveYearPlan) != 0) {
    return -1;
  }

  // 每人口 × Amount% 的城市产出修正；参数与 EFFECT_ADJUST_CITY_YIELD_MODIFIER 一致
  // （YieldType + Amount）。
  EffectDesc perPopulation = {};
  perPopulation.typeName = "EFFECT_YKKZ000_ADJUST_CITY_YIELD_PER_POPULATION_MODIFIER";
  perPopulation.templateEffect = "EFFECT_ADJUST_CITY_YIELD_MODIFIER";
  perPopulation.commonName = nullptr;
  perPopulation.description = nullptr;
  perPopulation.tags = nullptr;
  perPopulation.gameCapabilities = nullptr;
  perPopulation.contextInterfaces = nullptr;
  perPopulation.subjectInterfaces = nullptr;
  perPopulation.supportsRemove = -1;
  perPopulation.behavior = EffectBehavior::kCityYieldModifierPerPopulation;
  return host->registerEffectType(&perPopulation);
}

} // namespace

YKKZ000_PLUGIN_API int GetPlugin(Host* host) {
  if (host == nullptr || host->apiVersion < kHostApiVersion ||
      host->registerEffectType == nullptr) {
    return 0;
  }
  return registerEffects(host) == 0 ? 1 : 0;
}

YKKZ000_PLUGIN_API void DestroyPlugin() {
}
