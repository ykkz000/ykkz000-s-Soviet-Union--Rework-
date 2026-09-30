#pragma once

#include <ykkz000/bridge/host.h>

/// @file effect_module.h
/// @brief Plugin-side effect module interface.
/// @note Collapses all assembly and lifecycle of "one custom effect" into a single static
///       instance. The effect implementation lives in its own .cpp (assembling its own resident
///       EffectImpl/EffectDesc); plugin.cpp only iterates the manifest and knows no concrete
///       effect's name, fields, or toggles.
namespace ykkz000::plugin {

/// @brief The three-function interface of one custom effect module.
struct EffectModule {
  const char* name; ///< Diagnostic name.
  /// @brief Assembles the effect description, which the caller registers.
  /// @param[in] host Host service table.
  /// @return Pointer to the resident EffectDesc.
  const bridge::EffectDesc* (*describe)(const bridge::Host& host);
  /// @brief Context lifecycle callback; may be null.
  /// @param[in] event Event type.
  /// @param[in] context Context pointer.
  void (*on_context)(bridge::GameContextEvent event, void* context);
  void (*shutdown)(); ///< Unload cleanup; may be null.
};

} // namespace ykkz000::plugin
