#pragma once

/// @file object_cleanup.h
/// @brief Engine-object destruction cleanup: erases this mod's side-table records when the engine
///        destroys a City or Unit, so a recycled pointer/id cannot inherit stale data.
/// @note The Loader publishes the City/Unit destructor entry points (append-only EngineApi); this
///       module hooks them for the duration of a game context. The hooks read the object's identity
///       before forwarding to the original destructor and only erase plugin-owned records
///       afterwards; they never call engine functions on the half-destroyed object.
/// @note Shared by the city-yield and strength effect modules: whichever runs first installs the
///       hooks and the others are idempotent no-ops.
namespace ykkz000::plugin {

/// @brief Installs the City/Unit destructor hooks (idempotent, all-or-nothing).
/// @return true when the cleanup hooks are active.
/// @note Safe to call repeatedly on every context creation and from every effect module that wants
///       the cleanup; when a required engine entry is missing (or an install fails) the call logs
///       once and returns false, and the effect modules keep their current behavior (records are
///       reclaimed only at the next context switch).
[[nodiscard]] bool EnsureObjectCleanupHooks();

/// @brief Removes the City/Unit destructor hooks (idempotent).
/// @note Must run from the kDestroyed notification, which the Loader broadcasts after the real
///       context destroy; leaving the hooks installed through that destroy lets the destructors the
///       engine runs during teardown be observed first.
void RemoveObjectCleanupHooks();

} // namespace ykkz000::plugin
