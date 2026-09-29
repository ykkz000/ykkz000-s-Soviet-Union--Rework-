#pragma once

#include <cstdint>

#include <ykkz000/bridge.h>

#include "effect_module.h"

namespace ykkz000::plugin {

// 每市民 × Amount% 的城市产出修正。Apply/Remove 槽替换使用 bridge::ApplyFn 统一的
// 4 指针签名；城市产出模板实际只用前两个实参（self、city），余下忽略。
// Apply/Remove 只维护 CityExtra 侧表（每市民百分比），乘人口发生在引擎读取路径。
std::uint64_t CityYieldApply(void* self, void* a1, void* a2, void* a3);
std::uint64_t CityYieldRemove(void* self, void* a1, void* a2, void* a3);

// 该效果模块：自行装配 EffectImpl/EffectDesc（含 hook 安装入口），供清单登记与遍历。
const EffectModule* CityYieldModule();

// 停用 City::Instance::CalculateYield hook（幂等），并清空侧表与缓存。
void CityYieldUninstallHook();
// 上下文生命周期：created 时启用 hook 并清空侧表，destroyed 时停用并清空。
void CityYieldOnContext(bridge::GameContextEvent event, void* context);
// 插件卸载清理：停用 hook、清空缓存。
void CityYieldShutdown();

} // namespace ykkz000::plugin
