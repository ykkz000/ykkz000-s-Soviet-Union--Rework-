#pragma once

#include <span>

#include "effect_module.h"

namespace ykkz000::plugin {

// 全部自定义效果模块。新增效果 = 新建 effects/<name>.{h,cpp} + 在 effect_modules.cpp
// 的清单里加 1 行，plugin.cpp 无需改动。
std::span<const EffectModule* const> AllEffectModules();

} // namespace ykkz000::plugin
