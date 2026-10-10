#pragma once

#include <ykkz000/bridge/host.h>

/// @file effect.h
/// @brief Shared custom-effect interface used by every plugin that registers EffectTypes.
/// @note Provided by the ykkz000_plugin_api_effecttype plugin so consumers do not each re-declare the
///       interface. Each effect's implementation lives in its own .cpp under the consumer's
///       src/effects directory; the consumer's effects.h manifest only enumerates the resident
///       Effect pointers and the shared registration driver iterates them.
namespace ykkz000::plugin {

/// @brief The three-function interface of one custom effect.
struct Effect {
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
