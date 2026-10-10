#pragma once

/// @file object_lifecycle.h
/// @brief Registers this consumer's City/Unit side-table lifecycle with the persistence API.
/// @note The persistence API plugin owns the engine City/Unit constructor/destructor hooks and
///       notifies subscribers, so the consumer no longer installs hooks of its own. On construction
///       the consumer creates the PlayerExtras entry; on destruction it erases it, so a recycled
///       object pointer cannot inherit stale data.
namespace ykkz000::plugin {

/// @brief Subscribes the consumer's City/Unit lifecycle callbacks to the persistence API.
/// @return true when the subscription succeeded.
[[nodiscard]] bool RegisterObjectLifecycle();

} // namespace ykkz000::plugin
