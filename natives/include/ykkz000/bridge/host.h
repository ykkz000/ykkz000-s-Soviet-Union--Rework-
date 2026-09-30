#pragma once

#include <cstddef>
#include <cstdint>

/// @file host.h
/// @brief Pure C ABI boundary definitions between the host (loader) and plugins.
/// @note Uses only PODs, function pointers, and const char*; no std:: objects or exceptions are
///       passed across the DLL boundary.
namespace ykkz000::bridge {

/// @brief Host API version number.
/// @note Evolves under an "append-only" policy: new fields may only be appended, and the meaning
///       of existing fields never changes; plugins use this to decide whether the host supports
///       the capabilities they need.
inline constexpr std::uint32_t kHostApiVersion = 9;

struct Host;

/// @brief String hash: same algorithm as the engine's GameCore::Utilities::MakeHash.
using MakeHashFn          = std::uint32_t (*)(const char*);
/// @brief Log output: level is the log level, msg is UTF-8 text.
using LogFn               = void (*)(int level, const char* msg);
/// @brief Get the effect registry handle.
using GetEffectRegistryFn = void* (*)();

/// @brief Uniform signature for the Apply/Remove slots of an effect object.
/// @note The engine passes arguments according to the template's real signature; here they are
///       uniformly received as 4 pointer arguments (receiving extra arguments is harmless on x64),
///       and forwarded with the same arguments.
using ApplyFn = std::uint64_t (*)(void* self, void* a1, void* a2, void* a3);
/// @brief Signature for handler descriptor-table slot 0 (analyze) and slot 1 (apply).
using AnalyzeFn = void* (*)(void* self, void* args);

/// @brief The "implementation" a plugin provides for a custom EffectType.
/// @note All slots are optional: a null slot keeps the template behavior; the loader handles
///       everything generically and no longer recognizes any specific behavior.
struct EffectImpl {
  /// @brief Engine template Apply function pointer.
  /// @note The loader uses it (by function-pointer value) to locate the slot to replace in the
  ///       cloned vtable; it must be provided when apply/remove are non-null.
  const void* templateApply;
  /// @brief Engine template Remove function pointer (same semantics as templateApply).
  const void* templateRemove;
  ApplyFn     apply;        ///< Replaces the effect object's Apply slot; null = keep the template
  ApplyFn     remove;       ///< Replaces the effect object's Remove slot; null = keep the template
  AnalyzeFn   analyze;      ///< Replaces handler slot 0; null = forward to the template
  ApplyFn     handlerApply; ///< Replaces handler slot 1; null = forward to the template
  const char* label;        ///< Diagnostic name (may be null)
  void*       userData;     ///< Plugin context; the loader keeps it as-is and does not interpret it
};

/// @note GameEffects metadata is written by the engine via the template's GetTypeInfo; custom
///       metadata is not supported under the current mechanism, so no corresponding field is
///       provided here.

/// @brief Prepare entry point (hook installation function).
/// @param[in] userData The plugin context passed through as-is at registration time (see
///   EffectDesc::userData).
/// @return 0 on success; non-zero on failure.
/// @note Contract:
///       * Called once by host->registerEffectType after the factory/type have been registered
///         (registration phase).
///       * If it returns non-zero, registerEffectType rolls back this registration (the factory
///         object/type/impl record) and returns the error code; prepare itself must undo the hooks
///         it installed this time (the loader does not perform a half-way rollback).
///       * May be null (this effect needs no hook).
///       * Context-related re-installation/deactivation is not done here: it still goes through
///         Host::onGameContext (kCreated/kDestroyed) and DestroyPlugin (the plugin shuts itself
///         down).
using EffectPrepareFn = int (*)(void* userData);

/// @brief Describes a custom effect type to be registered.
struct EffectDesc {
  const char* typeName;       ///< Required: custom EffectType name
  const char* templateEffect; ///< Required: name of an existing effect whose behavior and parameter definitions are reused
  const EffectImpl* impl;     ///< null = fully reuse the template behavior
  EffectPrepareFn prepare;    ///< Called by registerEffectType; null = no hook needed
  void* userData;             ///< Passed to prepare as-is
};

/// @brief Register a custom effect type.
/// @return 0 on success, non-zero on failure.
using RegisterEffectTypeFn = int (*)(const EffectDesc*);

/// @brief Engine entry points: a read-only set of function pointers.
/// @note Evolves under an "append-only" policy: existing fields never change meaning.
struct EngineApi {
  void* effectApply;          ///< Effects::AdjustCityYieldModifier::Apply/Remove
  void* effectRemove;
  void* effectStrengthApply;  ///< Effects::AdjustPlayerStrengthModifier::Apply/Remove
  void* effectStrengthRemove;
  void* proposedCombatAdjust; ///< Combat-strength-modifier write point (authoritative source of the player id)
  void* changeYieldModifier;  ///< City::Instance::ChangeYieldModifier(YieldType, int)
  void* getPlayer;            ///< PlayerTypes -> Player::Instance* (no bounds check)
  void* getGameManager;       ///< -> GameManager* (+0x50 is the player vector)

  /// @note Added in v7 (append-only).
  /// @brief City yield read path: City::Instance::CalculateYield(YieldTypes, TypeHash, bool).
  /// @note Returns a TrackedValue (hidden sret: rcx=sret, rdx=city, r8=yield, r9=typeHash,
  ///       stack=flag). The base accumulator is at sret+0x10, the modifier sub-object at sret+0x30
  ///       (the modifier accumulator is at sret+0x40).
  void* cityCalculateYield;

  /// @note Added in v8 (append-only).
  /// @brief Modifier-detail append entry: TrackedValue::AddStep(this=modifier sub-object (out+0x30),
  ///       step, u32=0, u32=0, const char* tooltipKey).
  /// @note step is layout-identical to the sub-object (civ6::YieldValue: has_min/min/has_max/max/
  ///       value/flag/steps, 0x30 bytes); the engine writes only value and zeroes the remaining
  ///       fields.
  void* trackedValueAddStep;
};

/// @brief Install a hook.
/// @param[in] pluginHandle The plugin-ownership handle allocated by the host.
/// @param[in] target The target address.
/// @param[in] detour The replacement function.
/// @param[out] original Receives the original (trampoline) function pointer.
/// @return 0 means "the target is taken over by this plugin and usable" (including a repeated call
///   for an already-installed hook).
/// @note The single process-wide MinHook instance is owned by the loader's hook_service. Plugins
///       must not link MinHook themselves (two instances would corrupt each other's trampolines);
///       all installation/removal goes through here. Idempotent: it may be called repeatedly during
///       registration and on every game-context creation; a repeated call writes the existing
///       trampoline back into original, which the plugin can use to tell that the hook is still
///       valid. Switching the same target to a different detour is rejected (non-zero).
using HookInstallFn = int (*)(void* pluginHandle, void* target, void* detour, void** original);
/// @brief Remove a hook.
/// @param[in] pluginHandle The plugin-ownership handle allocated by the host.
/// @param[in] target The target address.
/// @return 0 means "the target is no longer taken over by this plugin" (including a repeated call
///   for an already-removed/never-registered hook).
/// @note "Already installed/already removed" are success semantics and must never be treated as a
///       failure -- otherwise the plugin would clear a trampoline that is still in effect.
using HookRemoveFn  = int (*)(void* pluginHandle, void* target);

/// @brief Validated memory read: expressed as "offset + byte count" to avoid passing type
///   information across the DLL boundary.
/// @param[in] base Base address.
/// @param[in] offset Field offset.
/// @param[in] bytes Number of bytes to read.
/// @param[out] out Receives the read result.
/// @return 1 on success; 0 on failure without modifying out.
using ReadFieldFn  = int (*)(const void* base, std::size_t offset, std::size_t bytes, void* out);
/// @brief Validated memory write (same semantics as ReadFieldFn).
/// @param[in] in Data to write; returns 0 on failure without modifying in.
using WriteFieldFn = int (*)(void* base, std::size_t offset, std::size_t bytes, const void* in);

/// @brief Determine whether a pointer is a valid engine-object candidate.
using IsCandidateFn = int (*)(const void* pointer);
/// @brief Determine whether [address, address+bytes) is readable.
using IsReadableFn  = int (*)(const void* address, std::size_t bytes);

/// @brief Game-context lifecycle events.
enum class GameContextEvent : std::int32_t {
  kCreated = 0,   ///< New game context created
  kDestroyed = 1, ///< Game context destroyed
};

/// @brief Context lifecycle notification.
/// @param[in] event kCreated or kDestroyed.
/// @param[in] context The context pointer provided by the host.
/// @note On kCreated the plugin installs and enables its own hooks and clears caches that became
///       invalid with the context; on kDestroyed it disables its hooks and cleans up caches. The
///       callback is wrapped in SEH by the loader.
using ContextListenerFn = void (*)(GameContextEvent event, void* context);

/// @brief The set of services the host provides to plugins.
/// @note Fields evolve under an "append-only" policy; apiVersion is used for version gating (see
///       kHostApiVersion).
struct Host {
  std::uint32_t        apiVersion;         ///< Host API version (see kHostApiVersion)
  MakeHashFn           makeHash;           ///< String hash
  RegisterEffectTypeFn registerEffectType; ///< Register a custom effect type
  LogFn                log;                ///< Log output
  void*                gameCoreModule;     ///< GameCore module base address
  GetEffectRegistryFn  getEffectRegistry;  ///< Get the effect registry

  /// @note Added in v5 (append-only).
  const EngineApi*     engine;             ///< Engine entry-point table
  HookInstallFn        installHook;        ///< Install a hook (idempotent)
  HookRemoveFn         removeHook;         ///< Remove a hook
  ReadFieldFn          readField;          ///< Validated memory read
  WriteFieldFn         writeField;         ///< Validated memory write
  IsCandidateFn        isCandidateObject;  ///< Engine-object candidate test
  IsReadableFn         isReadableRegion;   ///< Readable-region test
  ContextListenerFn    onGameContext;      ///< Context lifecycle notification
  void*                pluginHandle;       ///< Ownership handle the loader allocates for each plugin
};

/// @brief Plugin export entry point: called by the loader after loading the plugin to inject the
///   Host.
/// @param[in] host The host service table.
/// @return 0 on success, non-zero on failure.
using GetPluginFn     = int  (*)(Host* host);
/// @brief Plugin unload entry point: called by the loader before unloading the plugin to release
///   resources.
using DestroyPluginFn = void (*)();

} // namespace ykkz000::bridge

/// @brief Plugin export name "GetPlugin".
#define YKKZ000_PLUGIN_EXPORT_GETPLUGIN  "GetPlugin"
/// @brief Plugin export name "DestroyPlugin".
#define YKKZ000_PLUGIN_EXPORT_DESTROY    "DestroyPlugin"
