#pragma once

#include <cstdint>

#include <ykkz000/bridge.h>

#include "effect_module.h"

namespace ykkz000::plugin {

// 每宗主城邦 × Amount 的玩家单位战斗力修正。Apply/Remove 槽替换使用
// bridge::ApplyFn 统一的 4 指针签名（与战斗力模板的真实签名一致）。
std::uint64_t StrengthApply(void* self, void* a1, void* a2, void* a3);
std::uint64_t StrengthRemove(void* self, void* a1, void* a2, void* a3);

// 该效果模块：自行装配 EffectImpl/EffectDesc（含 hook 安装入口），供清单登记与遍历。
const EffectModule* StrengthModule();

// 停用“战斗力写入点缩放”hook（幂等）。
void StrengthUninstallHook();
// 上下文生命周期：created 时启用 hook，destroyed 时停用并清空缓存。
void StrengthOnContext(bridge::GameContextEvent event, void* context);
// 插件卸载清理：停用 hook、清空缓存。
void StrengthShutdown();

} // namespace ykkz000::plugin
