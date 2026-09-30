#pragma once

#include <cstdint>

#include <ykkz000/bridge.h>

#include "effect_module.h"

namespace ykkz000::plugin {

// 每宗主城邦 × Amount% 的城市产出修正。Apply/Remove 槽替换使用 bridge::ApplyFn 统一的
// 4 指针签名；城市产出模板实际只用前两个实参（self、city），余下忽略。
// Apply/Remove 只维护 CityExtra 侧表（每宗主百分比），乘宗主数发生在引擎读取路径——
// 该注入由 city-yield-per-population 模块的 CalculateYield hook 完成，本模块不安装
// 任何 hook（同一目标不能重复挂载，且该 hook 是两条修正的公共 host）。
std::uint64_t CityYieldPerSuzerainApply(void* self, void* a1, void* a2, void* a3);
std::uint64_t CityYieldPerSuzerainRemove(void* self, void* a1, void* a2, void* a3);

// 该效果模块：自行装配 EffectImpl/EffectDesc（prepare = null），供清单登记与遍历。
const EffectModule* CityYieldPerSuzerainModule();

} // namespace ykkz000::plugin
