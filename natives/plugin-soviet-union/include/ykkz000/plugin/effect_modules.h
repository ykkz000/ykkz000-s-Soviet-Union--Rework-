#pragma once

#include <span>

#include <ykkz000/plugin/effect_module.h>

/// @file effect_modules.h
/// @brief Manifest access entry point for all custom effect modules.
/// @note Adding an effect = create include/ykkz000/plugin/<name>.h + src/effects/<name>.cpp and add
///       one line to the manifest in src/core/effect_modules.cpp; plugin.cpp needs no changes.
namespace ykkz000::plugin {

/// @brief Get all custom effect modules.
/// @return Read-only view of the module manifest.
std::span<const EffectModule* const> AllEffectModules();

} // namespace ykkz000::plugin
