#include <ykkz000/plugin/adjust_player_strength_modifier_per_suzerain.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include <ykkz000/bridge/host.h>
#include <ykkz000/civ6/combat.h>
#include <ykkz000/civ6/effect.h>
#include <ykkz000/civ6/unit.h>
#include <ykkz000/extra/player_extra.h>

#include <ykkz000/plugin/engine_access.h>

// Unit strength modifier of "per suzerain city x Amount".
//
// The scaling happens at the engine's combat-strength write point (EngineApi::proposedCombatAdjust,
// i.e. GameEffects::ProposedCombat::AdjustPlayerStrengthModifier). There the engine passes the
// authoritative playerId it resolved itself and the target unit (ProposedCombat+0x00); the hook uses
// them to look up that unit's side-table entry, count suzerains, and replace amount with
// strength_per_suzerain x suzerain count; the template's +0x5C bookkeeping still uses the unscaled
// value, and the wrapper compensates the difference.
//
// The side table lives in PlayerExtras' units table (top-level key = player, sub-key = the unit id at
// Unit::Instance+0xB0). Each effect instance upserts keyed by its own pointer (self), and the
// aggregate is the sum over instances, so re-applying the same instance does not double-count
// (idempotent), different instances stack, and different units do not interfere -- this is exactly
// the fix for the previous version where a single per-player value was amplified by the unit count
// (N became MxN).
//
// The hook must forward every non-owning call losslessly (early-return when the active window is
// empty or amount == 0), because it is used by many built-in effects.
namespace ykkz000::plugin {
namespace {

// Defensive upper bound for the scaled Amount: above it the parse is treated as wrong and the
// template's original value is used.
constexpr int kMaxScaledStrengthAmount = 1000000;

using StrengthAccumulateFn =
    void (*)(civ6::GameEffects::ProposedCombat* target, int player_id, int amount);

std::mutex kAccumulateMutex;
StrengthAccumulateFn kAccumulateOriginal = nullptr;
void* kAccumulateTarget = nullptr;
bool kAccumulateInstalled = false;

// Hit log counter: print only the first 16 and every 4096th, to avoid flooding on every write.
std::atomic<long> kApplyLogCount{0};

// Whether this thread is currently running our effect object's template Apply/Remove (non-null means
// that self); plus the (scaled - original) accumulated in this window and the number of writes that
// reached the write point.
thread_local void* kApplyActive = nullptr;
thread_local int kScaledDelta = 0;
thread_local int kWriteCount = 0;
// The authoritative player captured first by the hook in this window (the top-level side-table key);
// -1 if not captured.
thread_local int kWindowPlayerId = -1;
// Whether the window only captures player and unit without scaling: Apply=false (the hook scales by
// the per-suzerain value), Remove=true (the template rolls back precisely from the already-modified
// +0x5C, so it must not scale again).
thread_local bool kWindowCaptureOnly = false;
// The target unit captured first by the hook in this window (the pointer is only for logging and
// consistency checks) and its unit id (the sub-table key); nullptr/-1 if not captured.
thread_local void* kWindowUnit = nullptr;
thread_local std::int32_t kWindowUnitId = -1;
// Per-player-type suzerain count cached within this Apply window: the suzerain relation cannot change
// within a single Apply, so it is safe to reuse; reset to -1 when the window opens. This avoids
// repeatedly walking the player vector when the template writes to the same player multiple times.
thread_local int kCountPlayerId = -1;
thread_local int kCountValue = -1;

// Unit side-table sub-key: Unit::Instance +0xB0 (release FUN_18005c4a0/FUN_1803a48d0 evidence).
// A failed or negative read is unusable (returns -1); in that case the unit side table is not
// maintained and the template Amount is used.
std::int32_t UnitIdOf(const void* unit) {
  if (unit == nullptr) {
    return -1;
  }
  const std::int32_t unit_id =
      TryReadOr(unit, &civ6::Unit::Instance::unit_id, std::int32_t{-1});
  return unit_id >= 0 ? unit_id : -1;
}

// Recompute the aggregate = sum over instances. Must be called after an upsert/erase so that
// strength_per_suzerain stays in sync with instances; if the aggregate exceeds the defensive bound,
// warn once (to catch re-inflation).
void RecomputeStrength(extra::UnitExtra& unit) {
  std::int32_t sum = 0;
  for (const auto& entry : unit.instances) {
    sum += entry.second;
  }
  unit.strength_per_suzerain = sum;
  if (sum > kMaxScaledStrengthAmount || sum < -kMaxScaledStrengthAmount) {
    static std::atomic<bool> kLoggedOverflow{false};
    if (!kLoggedOverflow.exchange(true)) {
      LogWarnF("extra: strength_per_suzerain=%d exceeds cap (unit_id=%d); "
               "side table may be inflated",
               sum, unit.unit_id);
    }
  }
}

void StrengthAccumulate_Hook(civ6::GameEffects::ProposedCombat* target, int player_id,
                             int amount) {
  if (kAccumulateOriginal == nullptr) {
    // Enabled but the trampoline is null: the engine's calls would be swallowed. This should not
    // happen; if it does it must be visible, otherwise all strength bonuses would only display
    // without taking effect.
    static std::atomic<bool> kLoggedNoTrampoline{false};
    if (!kLoggedNoTrampoline.exchange(true)) {
      LogError("strength: detour without trampoline; call dropped");
    }
    return;
  }
  if (kApplyActive == nullptr || amount == 0) { // Not our effect / empty value: forward as-is
    kAccumulateOriginal(target, player_id, amount);
    return;
  }
  ++kWriteCount;
  // Record the authoritative player in this window (multiple writes in one window should be for the
  // same player).
  if (kWindowPlayerId < 0) {
    kWindowPlayerId = player_id;
  } else if (kWindowPlayerId != player_id) {
    static std::atomic<bool> kLoggedPlayerMismatch{false};
    if (!kLoggedPlayerMismatch.exchange(true)) {
      LogWarnF("strength: window player mismatch (%d != %d)", kWindowPlayerId, player_id);
    }
  }
  // Capture the target unit and its unit id (both Apply and Remove need it to maintain the unit side
  // table). Also cross-check once that "target unit owner == playerId" to confirm the understanding
  // of the bookkeeping branch.
  if (kWindowUnit == nullptr) {
    civ6::Unit::Instance* unit = nullptr;
    (void)TryRead(target, &civ6::GameEffects::ProposedCombat::unit, unit);
    if (unit != nullptr) {
      kWindowUnit = unit;
      kWindowUnitId = UnitIdOf(unit);
      const civ6::PlayerTypes unit_owner =
          TryReadOr(unit, &civ6::Unit::Instance::owner, civ6::kInvalidPlayerType);
      static std::atomic<bool> kLoggedOwnerMismatch{false};
      if (civ6::PlayerTypeIndex(unit_owner) != player_id &&
          !kLoggedOwnerMismatch.exchange(true)) {
        LogWarnF("strength: hook target owner mismatch (playerId=%d unit+0x%zX=%d)",
                 player_id, offsetof(civ6::Unit::Instance, owner),
                 civ6::PlayerTypeIndex(unit_owner));
      }
    }
  }
  if (kWindowCaptureOnly) {
    // Remove window: only capture player/unit, do not scale (the template rolls back precisely from
    // the already-modified +0x5C).
    kAccumulateOriginal(target, player_id, amount);
    return;
  }
  void* player = PlayerById(player_id); // Prefer matching by +0xD8 (index != player type)
  if (player == nullptr || !IsRealPlayer(player)) {
    player = PlayerAtIndex(player_id);
  }
  int count = -1;
  if (player != nullptr) {
    count = (player_id == kCountPlayerId) ? kCountValue : CountSuzerainsOfPlayer(player);
    if (count >= 0) { // Record the cache for this Apply window (including 0: 0 is a valid count)
      kCountPlayerId = player_id;
      kCountValue = count;
    }
  }
  if (count < 0) {
    static std::atomic<bool> kLoggedCountUnresolved{false};
    if (!kLoggedCountUnresolved.exchange(true)) {
      LogWarnF("strength: hook count unresolved (playerId=%d)", player_id);
    }
    kAccumulateOriginal(target, player_id, amount);
    return;
  }
  // Prefer the unit side table (the authoritative per-suzerain value = sum over instances); if the
  // table has not been built yet (before the first Apply) or the module is disabled, fall back to the
  // template Amount so behavior is unchanged.
  int per_suzerain = amount;
  if (kWindowUnitId >= 0) {
    const std::int32_t stored =
        extra::PlayerExtras().FindUnitStrength(player_id, kWindowUnitId);
    if (stored != 0) {
      per_suzerain = stored;
      if (per_suzerain != amount) { // Normal case of multi-instance aggregation
        static std::atomic<bool> kLoggedTableDiff{false};
        if (!kLoggedTableDiff.exchange(true)) {
          LogDebugF("strength: extra per-suzerain=%d != amount=%d for player=%d unit=%d",
               per_suzerain, amount, player_id, kWindowUnitId);
        }
      }
    } else {
      static std::atomic<bool> kLoggedFallback{false};
      if (!kLoggedFallback.exchange(true)) {
        LogDebugF("strength: extra per-suzerain missing for player=%d unit=%d; "
               "fall back to amount=%d",
             player_id, kWindowUnitId, amount);
      }
    }
  } else {
    static std::atomic<bool> kLoggedNoUnitId{false};
    if (!kLoggedNoUnitId.exchange(true)) {
      LogWarnF("strength: unit id unavailable (unit=%p); unit side table not used",
               kWindowUnit);
    }
  }
  const long long scaled = static_cast<long long>(per_suzerain) * count;
  if (scaled > kMaxScaledStrengthAmount || scaled < -kMaxScaledStrengthAmount) {
    kAccumulateOriginal(target, player_id, amount);
    return;
  }
  kScaledDelta += static_cast<int>(scaled) - amount;
  const long hit = ++kApplyLogCount;
  if (hit <= 16 || (hit % 4096) == 0) {
    LogDebugF("strength: hook playerId=%d unit=%d amount=%d->%d suzerains=%d", player_id,
         kWindowUnitId, amount, static_cast<int>(scaled), count);
  }
  if ((hit % 4096) == 0) {
    // One-shot scale self-check: helps detect side-table inflation / key invalidation.
    static std::atomic<bool> kLoggedStats{false};
    if (!kLoggedStats.exchange(true)) {
      const auto stats = extra::PlayerExtras().CountStats();
      LogInfoF("extra: table players=%zu cities=%zu units=%zu", stats.players,
           stats.cities, stats.units);
    }
  }
  kAccumulateOriginal(target, player_id, static_cast<int>(scaled));
}

// Scaling is already done by the write-point hook; here we only manage state:
//   - open the window so the template is called with the original Amount (player resolution,
//     StackPercent/Scalar logic, etc. remain the template's job); within the window the hook captures
//     the authoritative playerId and target unit and scales by the side-table value;
//   - Apply: after the window closes, upsert this instance's Amount into the target unit's side-table
//     entry (instances[self] = amount, aggregate = sum), then compensate the template's +0x5C
//     unscaled value up to the scaled total (both the preview {Property} and Remove depend on it);
//   - Remove: the window only captures player/unit without scaling, erase this instance from the unit
//     side table (erase(self)) and recompute the aggregate; the template rolls back precisely from
//     +0x5C (already corrected to the scaled total at Apply time).
// Idempotent: instances is keyed by self with upsert, so the engine replaying Apply does not
// double-count.
struct WindowState {
  void* active;
  int delta;
  int writes;
  int count_player;
  int count_value;
  int window_player;
  bool capture_only;
  void* window_unit;
  std::int32_t window_unit_id;
};

// Open the window and return a snapshot of the old values for CloseWindow to restore (nesting is
// supported: the template may trigger another effect object's application on the same thread).
WindowState OpenWindow(void* self, bool capture_only) {
  const WindowState previous{kApplyActive,   kScaledDelta,     kWriteCount,
                             kCountPlayerId, kCountValue,      kWindowPlayerId,
                             kWindowCaptureOnly, kWindowUnit, kWindowUnitId};
  kApplyActive = self;
  kScaledDelta = 0;
  kWriteCount = 0;
  kCountPlayerId = -1; // New window: invalidate the suzerain count cache
  kCountValue = -1;
  kWindowPlayerId = -1;
  kWindowCaptureOnly = capture_only;
  kWindowUnit = nullptr;
  kWindowUnitId = -1;
  return previous;
}

void CloseWindow(const WindowState& previous) {
  kApplyActive = previous.active;
  kScaledDelta = previous.delta;
  kWriteCount = previous.writes;
  kCountPlayerId = previous.count_player;
  kCountValue = previous.count_value;
  kWindowPlayerId = previous.window_player;
  kWindowCaptureOnly = previous.capture_only;
  kWindowUnit = previous.window_unit;
  kWindowUnitId = previous.window_unit_id;
}

std::uint64_t ApplyPerSuzerain(void* self, void* a1, void* a2, void* a3, int sign) {
  const bridge::EngineApi* engine = Context().engine;
  if (!IsCandidateObject(self) || engine == nullptr || engine->effectStrengthApply == nullptr ||
      engine->effectStrengthRemove == nullptr) {
    return 0;
  }
  // Forward the same 4 arguments as the engine; the Amount scaling only happens at the write-point
  // hook inside the Apply window.
  const auto call_template = [&](void* s) -> std::uint64_t {
    return sign < 0
               ? reinterpret_cast<bridge::ApplyFn>(engine->effectStrengthRemove)(s, a1, a2, a3)
               : reinterpret_cast<bridge::ApplyFn>(engine->effectStrengthApply)(s, a1, a2, a3);
  };

  const WindowState previous = OpenWindow(self, sign < 0);
  const std::uint64_t result = call_template(self);
  const int writes = kWriteCount;
  const int delta = kScaledDelta;
  const int window_player = kWindowPlayerId;
  const std::int32_t window_unit_id = kWindowUnitId;
  CloseWindow(previous);

  if (writes == 0) {
    // Not a single write reached the write point within the window: the hook/entry is not in effect
    // (suspect a stale DLL or a wrong entry point); or the template Remove's Amount (+0x5C) is
    // exactly 0, making the hook early-return.
    static std::atomic<bool> kLoggedHookMiss{false};
    if (!kLoggedHookMiss.exchange(true)) {
      LogErrorF("strength: hook never fired (self=%p sign=%d)", self, sign);
    }
    return result;
  }
  if (window_player < 0) {
    return result;
  }
  // Maintain the unit side table: Apply upserts / Remove erases, then recompute the aggregate (sum).
  if (window_unit_id >= 0) {
    std::int32_t amount = 0;
    if (TryRead(self, &civ6::AdjustPlayerStrengthModifier::amount, amount)) {
      if (sign < 0) {
        extra::PlayerExtras().EditUnit(window_player, window_unit_id,
                                       [&](extra::UnitExtra& unit) {
                                         unit.instances.erase(self);
                                         RecomputeStrength(unit);
                                       });
        extra::PlayerExtras().EraseIfEmptyUnit(window_player, window_unit_id);
      } else {
        extra::PlayerExtras().EditUnit(window_player, window_unit_id,
                                       [&](extra::UnitExtra& unit) {
                                         unit.instances[self] = amount;
                                         RecomputeStrength(unit);
                                       });
        static std::atomic<bool> kLoggedFirstExtra{false};
        if (!kLoggedFirstExtra.exchange(true)) {
          LogInfoF("strength: extra per-suzerain=%d for player=%d unit=%d (self=%p)",
               amount, window_player, window_unit_id, self);
        }
      }
    } else {
      static std::atomic<bool> kLoggedNoAmount{false};
      if (!kLoggedNoAmount.exchange(true)) {
        LogWarnF("strength: cannot read effect Amount (self=%p); unit side table "
                 "not maintained",
                 self);
      }
    }
  }
  if (delta != 0) {
    std::int32_t applied_total = 0;
    if (TryRead(self, &civ6::AdjustPlayerStrengthModifier::applied_total, applied_total)) {
      (void)TryWrite(self, &civ6::AdjustPlayerStrengthModifier::applied_total,
                     applied_total + delta);
    }
  }
  return result;
}

bool InstallHooksOnce() {
  const bridge::Host* host = Context().host;
  const bridge::EngineApi* engine = Context().engine;
  if (host == nullptr || host->installHook == nullptr || host->removeHook == nullptr ||
      engine == nullptr || engine->proposedCombatAdjust == nullptr) {
    LogWarn("strength: combat write point unavailable; effect degrades to the template "
            "amount (no per-suzerain scaling)");
    return false;
  }
  std::lock_guard<std::mutex> guard(kAccumulateMutex);
  // Idempotent fast path: already installed with a usable trampoline counts as success. This avoids
  // re-enabling an enabled hook and avoids clearing a still-effective trampoline when the loader
  // wrongly reports failure.
  if (kAccumulateInstalled && kAccumulateOriginal != nullptr && kAccumulateTarget != nullptr) {
    return true;
  }
  void* const target = engine->proposedCombatAdjust;
  const int status = host->installHook(host->pluginHandle, target,
                                       reinterpret_cast<void*>(&StrengthAccumulate_Hook),
                                       reinterpret_cast<void**>(&kAccumulateOriginal));
  if (status != 0) {
    LogErrorF("strength: accumulate hook install -> %d", status);
    // Undo the takeover first so the hook stops intercepting, then clear the trampoline; reversing
    // the order would leave an "enabled but null trampoline" state where the detour swallows every
    // engine call.
    (void)host->removeHook(host->pluginHandle, target);
    kAccumulateOriginal = nullptr;
    kAccumulateInstalled = false;
    kAccumulateTarget = nullptr;
    return false;
  }
  kAccumulateTarget = target;
  kAccumulateInstalled = true;
  LogInfoF("strength: combat write point hook installed target=%p detour=%p trampoline=%p",
       kAccumulateTarget, reinterpret_cast<void*>(&StrengthAccumulate_Hook),
       reinterpret_cast<void*>(kAccumulateOriginal));
  return true;
}

void ResetWindow() {
  kApplyActive = nullptr;
  kScaledDelta = 0;
  kWriteCount = 0;
  kCountPlayerId = -1;
  kCountValue = -1;
  kWindowPlayerId = -1;
  kWindowCaptureOnly = false;
  kWindowUnit = nullptr;
  kWindowUnitId = -1;
}

// Stop the "combat write point scaling" hook (idempotent).
void UninstallHook() {
  const bridge::Host* host = Context().host;
  std::lock_guard<std::mutex> guard(kAccumulateMutex);
  if (kAccumulateInstalled && host != nullptr && host->removeHook != nullptr &&
      kAccumulateTarget != nullptr) {
    (void)host->removeHook(host->pluginHandle, kAccumulateTarget);
    kAccumulateInstalled = false;
  }
  ResetWindow();
  // Disable/unload: clear the shared side table to avoid residue across games (the city-yield module
  // clears it too; idempotent).
  extra::PlayerExtras().Clear();
}

// Context lifecycle: enable the hook on created; disable and clear the cache on destroyed.
void OnContext(bridge::GameContextEvent event, void* /*context*/) {
  if (event == bridge::GameContextEvent::kCreated) {
    (void)InstallHooksOnce();
    ResetWindow();
    extra::PlayerExtras().Clear(); // New context: old player/city/unit keys are all invalid
    return;
  }
  UninstallHook();
}

// Plugin unload cleanup: stop the hook and clear the cache.
void Shutdown() { UninstallHook(); }

// Apply/Remove slots (bridge::ApplyFn signature).
std::uint64_t Apply(void* self, void* a1, void* a2, void* a3) {
  return ApplyPerSuzerain(self, a1, a2, a3, 1);
}

std::uint64_t Remove(void* self, void* a1, void* a2, void* a3) {
  return ApplyPerSuzerain(self, a1, a2, a3, -1);
}

// This module's hook install entry: a capture-less lambda (usable as a function pointer), with state
// kept at file scope for the detour and the context callback to share. Returns 0 when the write point
// is unavailable: the effect degrades to the template value and the whole registration does not fail
// because of it.
const bridge::EffectPrepareFn kStrengthPrepare = +[](void* /*userData*/) -> int {
  (void)InstallHooksOnce();
  return 0;
};

// Resident assembly objects: the loader keeps the impl pointer, so they must not be temporaries.
bridge::EffectImpl g_impl = {};
bridge::EffectDesc g_desc = {};
bool g_described = false;

// Assemble this module's EffectImpl/EffectDesc; returns the resident desc for plugin.cpp to
// register.
const bridge::EffectDesc* Describe(const bridge::Host& host) {
  (void)host;
  if (g_described) {
    return &g_desc;
  }
  g_desc.typeName = "EFFECT_YKKZ000_ADJUST_PLAYER_STRENGTH_MODIFIER_PER_SUZERAIN";
  g_desc.templateEffect = "EFFECT_ADJUST_PLAYER_STRENGTH_MODIFIER";
#if !defined(DISABLE_CUSTOM_BEHAVIOR) && !defined(DISABLE_STRENGTH_PER_SUZERAIN)
  const bridge::EngineApi* engine = host.engine;
  if (engine == nullptr) {
    return nullptr;
  }
  g_impl.templateApply = engine->effectStrengthApply;
  g_impl.templateRemove = engine->effectStrengthRemove;
  g_impl.apply = &Apply;
  g_impl.remove = &Remove;
  g_impl.label = "player-strength-per-suzerain";
  g_desc.impl = &g_impl;
  g_desc.prepare = kStrengthPrepare;
#else
  g_desc.impl = nullptr;    // Custom behavior disabled: degrade to the template behavior
  g_desc.prepare = nullptr; // No hook
#endif
  g_described = true;
  return &g_desc;
}

const EffectModule g_module = {"player-strength-per-suzerain", &Describe, &OnContext,
                               &Shutdown};

} // namespace

const EffectModule* StrengthModule() { return &g_module; }

} // namespace ykkz000::plugin
