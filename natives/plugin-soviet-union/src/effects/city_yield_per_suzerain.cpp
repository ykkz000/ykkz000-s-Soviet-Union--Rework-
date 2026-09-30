#include "city_yield_per_suzerain.h"

#include <cstdint>
#include <limits>
#include <vector>

#include <ykkz000/civ6/effect.h>
#include <ykkz000/extra/player_extra.h>

#include "city_yield_common.h"
#include "engine_access.h"

// 每宗主城邦 × Amount% 的城市产出修正（读取时乘宗主数）。
//
// 与 city-yield-per-population 共用同一张侧表（CityExtra）与同一个注入点：Apply/Remove
// 只把“每宗主百分比”聚合进 per_suzerain_percent[]（FixedPoint<16>，0x10000 == +1%/宗主），
// 真正的 (值 × 当前宗主数) >> 8 折算发生在 city-yield-per-population 模块持有的
// CalculateYield hook 中，因此宗主数变化被下一次读取自然跟随，无需在此记账。
//
// 本模块不安装 hook（prepare = null）：CalculateYield 的 MinHook 由既有模块独占，同一
// 目标不能重复挂载；侧表的清理由上下文事件统一负责（见 city_yield_per_population）。若
// 既有模块被 YKKZ000_DISABLE_CUSTOM_BEHAVIOR 禁用，本效果也不会注入（依赖 host）。
namespace ykkz000::plugin {
namespace {

// 把条目按 sign(±1) 聚合进侧表的 per_suzerain_percent[]：
//   per_suzerain_percent += sign * Amount * kPercentUnit。
void ApplyPerSuzerainEntries(void* self, void* city, int sign) {
  if (self == nullptr || city == nullptr) {
    return;
  }
  std::vector<EffectEntry> entries;
  if (!ReadEffectEntries(self, entries)) {
    return;
  }
  CityRef ref;
  if (!CityRefOf(city, ref)) {
    return;
  }
  constexpr std::int64_t kPercentMin = std::numeric_limits<std::int32_t>::min();
  constexpr std::int64_t kPercentMax = std::numeric_limits<std::int32_t>::max();
  extra::PlayerExtras().EditCity(
      ref.player_id, ref.city_id, ref.player_id, [&](extra::CityExtra& extra) {
        for (const EffectEntry& entry : entries) {
          if (entry.yield_type < 0 ||
              entry.yield_type >= static_cast<int>(civ6::kMaxYields)) {
            continue;
          }
          if (entry.amount < -kMaxPlausibleAmount || entry.amount > kMaxPlausibleAmount) {
            continue;
          }
          const std::int64_t delta =
              static_cast<std::int64_t>(entry.amount) * kPercentUnit * sign;
          std::int64_t updated = static_cast<std::int64_t>(
                                     extra.per_suzerain_percent[entry.yield_type]) +
                                 delta;
          if (updated > kPercentMax) {
            updated = kPercentMax;
          } else if (updated < kPercentMin) {
            updated = kPercentMin;
          }
          extra.per_suzerain_percent[entry.yield_type] =
              static_cast<std::int32_t>(updated);
          if (entry.yield_type + 1 > extra.yield_count) {
            extra.yield_count = entry.yield_type + 1;
          }
        }
      });
  // 与既有模块一致：侧表变化后清引擎的城市产出缓存 + 发送 0 增量“产出已变化”通知，
  // 并令 TLS 快照失效（EditCity 已写入，读取路径须看到新值）。
  InvalidateAndNotifyCityYield(city, entries);
  if (sign < 0) {
    extra::PlayerExtras().EraseIfEmptyCity(ref.player_id, ref.city_id);
  }
  InvalidateCityExtraSnapshotCache();
}

// 常驻装配对象：loader 长期持有 impl 指针，不可为临时对象。
bridge::EffectImpl g_impl = {};
bridge::EffectDesc g_desc = {};
bool g_described = false;

// 装配本模块的 EffectImpl/EffectDesc；返回常驻的 desc 供 plugin.cpp 登记。
const bridge::EffectDesc* Describe(const bridge::Host& host) {
  (void)host;
  if (g_described) {
    return &g_desc;
  }
  g_desc.typeName = "EFFECT_YKKZ000_ADJUST_CITY_YIELD_MODIFIER_PER_SUZERAIN";
  g_desc.templateEffect = "EFFECT_ADJUST_CITY_YIELD_MODIFIER";
#if !defined(YKKZ000_DISABLE_CUSTOM_BEHAVIOR)
  const bridge::EngineApi* engine = host.engine;
  if (engine == nullptr) {
    return nullptr;
  }
  g_impl.templateApply = engine->effectApply;
  g_impl.templateRemove = engine->effectRemove;
  g_impl.apply = &CityYieldPerSuzerainApply;
  g_impl.remove = &CityYieldPerSuzerainRemove;
  g_impl.label = "city-yield-per-suzerain";
  g_desc.impl = &g_impl;
  // 无 hook：CalculateYield 的注入由既有 city-yield 模块独占持有。
  g_desc.prepare = nullptr;
#else
  g_desc.impl = nullptr;    // 关闭自定义行为：退化为模板行为
  g_desc.prepare = nullptr; // 不装 hook
#endif
  g_described = true;
  return &g_desc;
}

const EffectModule g_module = {"city-yield-per-suzerain", &Describe, nullptr, nullptr};

} // namespace

const EffectModule* CityYieldPerSuzerainModule() { return &g_module; }

std::uint64_t CityYieldPerSuzerainApply(void* self, void* a1, void* a2, void* a3) {
  (void)a2;
  (void)a3;
  ApplyPerSuzerainEntries(self, a1, 1);
  return static_cast<std::uint64_t>(1);
}

std::uint64_t CityYieldPerSuzerainRemove(void* self, void* a1, void* a2, void* a3) {
  (void)a2;
  (void)a3;
  ApplyPerSuzerainEntries(self, a1, -1);
  return static_cast<std::uint64_t>(1);
}

} // namespace ykkz000::plugin
