#include "adjust_city_yield_per_population_modifier.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <vector>

#include <ykkz000/bridge/host.h>
#include <ykkz000/civ6/city.h>
#include <ykkz000/civ6/effect.h>
#include <ykkz000/civ6/tracked_value.h>
#include <ykkz000/extra/player_extra.h>

#include "city_yield_common.h"
#include "engine_access.h"

// Injection host for city-yield modifiers: append two "multiply at read time" modifiers on the
// engine's yield read path City::Instance::CalculateYield --
//   1) per citizen x Amount% : percent[y] x population;
//   2) per suzerain city x Amount% : per_suzerain_percent[y] x current suzerain count.
// Both aggregate into the PlayerExtras city side table (top-level key = player id, sub-key =
// city_id); each module owns its own writes (this module = per citizen,
// adjust_city_yield_modifier_per_suzerain = per suzerain). This module owns the single
// CalculateYield hook; the other module only writes the side table and installs no hook.
//
// Rationale: Apply/Remove only aggregate the per-citizen/per-suzerain percentage into the side
// table; the actual multiplication happens on the engine's yield read path -- before returning, a
// modifier step is appended the way the engine itself does:
// TrackedValue::AddStep(out+0x30, step, 0, 0, tooltipKey),
//   step.value = (percent[yield] * population + per_suzerain[yield] * suzerains) >> 8.
// Population/suzerain changes and yield recomputation therefore always use the latest values, with
// no ChangePopulation hook and no "already applied baseline" bookkeeping; repeated instances with
// the same parameters are symmetric += / -=, with no ambiguity and no drift.
//
// Injection point (release build RVA 0x12FF20):
//   TrackedValue* City::Instance::CalculateYield(City* this, TrackedValue* out,
//                                                int yield, int typeHash, bool record_steps)
//   out is the sret (the function writes it and returns it in RAX): base accum at out+0x10;
//   modifier sub-object at out+0x30, its accumulated value at out+0x40 (FixedPoint<8>, 256 == +1%,
//   final yield = base * (1 + modifier / 25600)).
//
// Using AddStep instead of writing out+0x40 directly keeps the number and the detail rows from the
// same source: on the record_steps read path (release FUN_180132B60, the city-yield detail panel)
// the engine copies the whole TrackedValue together with its detail steps, so our modifier shows up
// in the panel detail; writing the accumulator directly would only change the number, with no
// detail row.
namespace ykkz000::plugin {
namespace {

// Offsets inside the TrackedValue returned by CalculateYield: the modifier sub-object and its
// accumulated value.
constexpr std::size_t kModifierPartOffset = offsetof(civ6::TrackedValue, modifier);
constexpr std::size_t kValueOffset = offsetof(civ6::YieldValue, value);
constexpr std::size_t kModifierAccumulatedOffset = kModifierPartOffset + kValueOffset;

using CalculateYieldFn = void* (*)(void* city, void* out, int yield, int type_hash,
                                   bool record_steps);

// The engine's own entry TrackedValue::AddStep (release RVA 0x12FC10): merges a modifier into the
// accumulated value of the modifier sub-object and, when record_steps is set, drops a detail row
// carrying the tooltip key. Arguments 3/4 are always 0 as the engine passes them (see the
// convenience overload 0x12FCE0); argument 5 passes the localization key on the stack.
using AddStepFn = void (*)(void* modifier_part, civ6::YieldValue* step, std::uint32_t arg3,
                           std::uint32_t arg4, const char* tooltip_key);

// Reuse the same tooltip key as the engine's game-effects modifier: no new localized text is
// needed, and the detail row is semantically correct (this effect is itself a city-yield modifier).
constexpr char kModifierTooltipKey[] =
    "LOC_CITY_YIELD_FROM_MODIFIER_GAMEEFFECTS_TOOLTIP";

std::mutex kHookMutex;
CalculateYieldFn kCalculateYieldOriginal = nullptr;
void* kCalculateYieldTarget = nullptr;
bool kCalculateYieldInstalled = false;

// Runtime dry-run (log only, do not modify): enabled by the environment variable
// YKKZ000_CITY_YIELD_DRY_RUN, used to confirm the TrackedValue layout against real values before
// release. Off by default.
bool DryRunEnabled() {
  static const bool enabled = []() {
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, "YKKZ000_CITY_YIELD_DRY_RUN") != 0 || value == nullptr) {
      return false;
    }
    const bool on = value[0] != '\0' && value[0] != '0';
    std::free(value);
    return on;
  }();
  return enabled;
}

// Aggregate entries into the side table's percent[] by sign(+-1): percent += sign * Amount *
// kPercentUnit.
void ApplyEntries(void* self, void* city, int sign) {
  if (self == nullptr || city == nullptr) {
    return;
  }
  std::vector<EffectEntry> entries;
  if (!ReadEffectEntries(self, entries)) {
    return;
  }
  CityRef ref;
  if (!CityRefOf(city, ref)) {
    return;
  }
  static std::atomic<bool> kLoggedFirstApply{false};
  if (!kLoggedFirstApply.exchange(true)) {
    LogF(1, "city-yield: first apply self=%p city=%p owner=%d entries=%zu", self, city,
         ref.player_id, entries.size());
    for (const EffectEntry& entry : entries) {
      LogF(2, "city-yield: entry yield=%d amount=%d", entry.yield_type, entry.amount);
    }
  }
  constexpr std::int64_t kPercentMin = std::numeric_limits<std::int32_t>::min();
  constexpr std::int64_t kPercentMax = std::numeric_limits<std::int32_t>::max();
  extra::PlayerExtras().EditCity(
      ref.player_id, ref.city_id, ref.player_id, [&](extra::CityExtra& extra) {
        for (const EffectEntry& entry : entries) {
          if (entry.yield_type < 0 ||
              entry.yield_type >= static_cast<int>(civ6::kMaxYields)) {
            continue;
          }
          if (entry.amount < -kMaxPlausibleAmount || entry.amount > kMaxPlausibleAmount) {
            continue;
          }
          const std::int64_t delta =
              static_cast<std::int64_t>(entry.amount) * kPercentUnit * sign;
          std::int64_t updated =
              static_cast<std::int64_t>(extra.percent[entry.yield_type]) + delta;
          if (updated > kPercentMax) {
            updated = kPercentMax;
          } else if (updated < kPercentMin) {
            updated = kPercentMin;
          }
          extra.percent[entry.yield_type] = static_cast<std::int32_t>(updated);
          if (entry.yield_type + 1 > extra.yield_count) {
            extra.yield_count = entry.yield_type + 1;
          }
        }
      });
  // After the side table changes, invalidate the engine's city-yield cache and send a zero-delta
  // "yield changed" notification (details in city_yield_common: clear the valid flag in
  // city+0x1950 + ChangeYieldModifier(city, yield, 0)). Idempotent: repeated Apply/Remove just
  // clears again. This must happen in Apply/Remove, never inside the CalculateYield hook
  // (otherwise every read would trigger a recompute/notification).
  InvalidateAndNotifyCityYield(city, entries);
  if (sign < 0) {
    extra::PlayerExtras().EraseIfEmptyCity(ref.player_id, ref.city_id);
  }
  InvalidateCityExtraSnapshotCache();
}

// Hit counters: warn once at level 0 when hits stay at zero for a long time (avoid silent failure).
std::atomic<long> kCallCount{0};
std::atomic<long> kHitCount{0};

// Hit probe: rate-limited logging of key quantities, to tell whether numbers reach the engine.
void LogHit(const CityRef& ref, int yield, int population, int suzerains,
            std::int64_t delta, long hit) {
  LogF(2, "city-yield: hit#%ld yield=%d pop=%d suzerains=%d player=%d city=%d delta=%lld",
       hit, yield, population, suzerains, ref.player_id, ref.city_id,
       static_cast<long long>(delta));
}

void* CalculateYield_Hook(void* city, void* out, int yield, int type_hash,
                          bool record_steps) {
  if (kCalculateYieldOriginal == nullptr) {
    // Enabled but the trampoline is null: the engine's yield read calls would be swallowed. This
    // should not happen; if it does it must be visible.
    static std::atomic<bool> kLoggedNoTrampoline{false};
    if (!kLoggedNoTrampoline.exchange(true)) {
      Log(0, "city-yield: detour without trampoline; call dropped");
    }
    return out;
  }
  void* returned = kCalculateYieldOriginal(city, out, yield, type_hash, record_steps);
  if (city == nullptr || returned == nullptr ||
      yield < 0 || yield >= static_cast<int>(civ6::kMaxYields)) {
    return returned;
  }

  const long call = ++kCallCount;
  // If Apply already happened (the side table was written) yet this hook never hits, the injection
  // point or the key must be wrong: warn once.
  if (call >= 8192 && kHitCount.load(std::memory_order_relaxed) == 0 &&
      CityExtraSnapshotGeneration() != 0) {
    static std::atomic<bool> kLoggedNeverHit{false};
    if (!kLoggedNeverHit.exchange(true)) {
      Log(0, "city-yield: CalculateYield hook fired but never applied (8192 calls); "
             "side table or key may be wrong");
    }
  }
  CityRef ref;
  if (!CityRefOf(city, ref)) {
    return returned;
  }
  const extra::CityExtra* extra = LookupCityExtra(ref.player_id, ref.city_id);
  if (extra == nullptr || yield >= extra->yield_count) {
    return returned;
  }
  const std::int32_t percent = extra->percent[yield];
  const std::int32_t per_suzerain = extra->per_suzerain_percent[yield];
  if (percent == 0 && per_suzerain == 0) {
    return returned;
  }

  std::int32_t population = -1;
  std::int64_t delta = 0;
  if (percent != 0) {
    population = TryReadOr(city, &civ6::City::Instance::population, std::int32_t{-1});
    if (population < 0 || population > kMaxPlausiblePopulation) {
      static std::atomic<bool> kLoggedBadPopulation{false};
      if (!kLoggedBadPopulation.exchange(true)) {
        LogF(0, "city-yield: implausible population %d (city=%p); skipping write",
             population, city);
      }
      return returned;
    }
    delta = (static_cast<std::int64_t>(percent) * population) >> 8;
  }
  int suzerains = 0;
  if (per_suzerain != 0) {
    // The suzerain count is read through a TTL cache (walking the player vector is expensive on the
    // hot path).
    suzerains = SuzerainCountForPlayer(ref.player_id);
    if (suzerains > 0) {
      delta += (static_cast<std::int64_t>(per_suzerain) * suzerains) >> 8;
    } else {
      suzerains = 0;
    }
  }
  std::int32_t modifier = 0;
  if (!TryReadAt(returned, kModifierAccumulatedOffset, modifier)) {
    return returned;
  }
  if (delta == 0) {
    return returned;
  }
  if (delta < std::numeric_limits<std::int32_t>::min() ||
      delta > std::numeric_limits<std::int32_t>::max()) {
    return returned; // percent/population/suzerains out of range: skip instead of writing overflow
  }
  const long hit = ++kHitCount;
  const bool log_this = hit <= 16 || (hit % 4096) == 0;

  if (DryRunEnabled()) {
    static std::atomic<bool> kLoggedDryRun{false};
    if (!kLoggedDryRun.exchange(true)) {
      LogF(1, "city-yield: dry-run enabled; would add %lld to +0x%zX (was %d)",
           static_cast<long long>(delta),
           static_cast<std::size_t>(kModifierAccumulatedOffset), modifier);
    }
    if (log_this) {
      LogHit(ref, yield, population, suzerains, delta, hit);
    }
    return returned;
  }

  // Prefer the engine's native AddStep: the value merges into modifier.value and, when record_steps
  // is set, a visible detail row is emitted, exactly like the engine's own game-effects / religion
  // / governor-title modifiers.
  const bridge::EngineApi* engine = Context().engine;
  const auto add_step = engine != nullptr && engine->trackedValueAddStep != nullptr
                            ? reinterpret_cast<AddStepFn>(engine->trackedValueAddStep)
                            : nullptr;
  if (add_step != nullptr) {
    civ6::YieldValue step = {};
    step.value = static_cast<std::int32_t>(delta);
    void* const modifier_part =
        static_cast<std::uint8_t*>(returned) + kModifierPartOffset;
    add_step(modifier_part, &step, 0, 0, kModifierTooltipKey);
  } else {
    // Degraded path: when the engine does not expose AddStep (older builds), add directly to the
    // modifier accumulator; there is no detail row.
    const std::int64_t updated = static_cast<std::int64_t>(modifier) + delta;
    if (updated < std::numeric_limits<std::int32_t>::min() ||
        updated > std::numeric_limits<std::int32_t>::max()) {
      return returned;
    }
    (void)TryWriteAt(returned, kModifierAccumulatedOffset,
                     static_cast<std::int32_t>(updated));
  }
  if (log_this) {
    LogHit(ref, yield, population, suzerains, delta, hit);
  }
  return returned;
}

bool InstallHooksOnce() {
  const bridge::Host* host = Context().host;
  const bridge::EngineApi* engine = Context().engine;
  if (host == nullptr || host->installHook == nullptr || host->removeHook == nullptr ||
      engine == nullptr || engine->cityCalculateYield == nullptr) {
    Log(0, "city-yield: CalculateYield entry unavailable; per-population modifier "
           "will not apply");
    return false;
  }
  std::lock_guard<std::mutex> guard(kHookMutex);
  // Idempotent fast path: already installed with a usable trampoline counts as success. This avoids
  // re-enabling an enabled hook and avoids clearing a still-effective trampoline when the loader
  // wrongly reports failure.
  if (kCalculateYieldInstalled && kCalculateYieldOriginal != nullptr &&
      kCalculateYieldTarget != nullptr) {
    return true;
  }
  void* const target = engine->cityCalculateYield;
  const int status = host->installHook(host->pluginHandle, target,
                                       reinterpret_cast<void*>(&CalculateYield_Hook),
                                       reinterpret_cast<void**>(&kCalculateYieldOriginal));
  if (status != 0) {
    LogF(0, "city-yield: CalculateYield hook install -> %d", status);
    // Undo the takeover first so the hook stops intercepting, then clear the trampoline; reversing
    // the order would leave an "enabled but null trampoline" state where the detour swallows every
    // engine yield read.
    (void)host->removeHook(host->pluginHandle, target);
    kCalculateYieldOriginal = nullptr;
    kCalculateYieldInstalled = false;
    kCalculateYieldTarget = nullptr;
    return false;
  }
  kCalculateYieldTarget = target;
  kCalculateYieldInstalled = true;
  LogF(1, "city-yield: CalculateYield hook installed target=%p detour=%p trampoline=%p",
       kCalculateYieldTarget, reinterpret_cast<void*>(&CalculateYield_Hook),
       reinterpret_cast<void*>(kCalculateYieldOriginal));
  return true;
}

// Clear the side table and invalidate the TLS snapshot cache on all threads (the generation bump is
// visible to everyone).
// Note: PlayerExtras() is shared with the strength module; repeated Clear is idempotent (clearing
// an empty table), but it should only be called at global-invalidation points (context
// creation/destruction or plugin unload).
void ClearExtras() {
  extra::PlayerExtras().Clear();
  ResetCityYieldCommonCaches();
}

// Stop the City::Instance::CalculateYield hook (idempotent) and clear the side table and caches.
void UninstallHook() {
  const bridge::Host* host = Context().host;
  std::lock_guard<std::mutex> guard(kHookMutex);
  if (kCalculateYieldInstalled && host != nullptr && host->removeHook != nullptr &&
      kCalculateYieldTarget != nullptr) {
    (void)host->removeHook(host->pluginHandle, kCalculateYieldTarget);
    kCalculateYieldInstalled = false;
  }
  ClearExtras();
}

// Context lifecycle: enable the hook and clear the side table on created; disable and clear on
// destroyed.
void OnContext(bridge::GameContextEvent event, void* /*context*/) {
  if (event == bridge::GameContextEvent::kCreated) {
    (void)InstallHooksOnce();
    ClearExtras(); // New context: old city keys are all invalid
    return;
  }
  UninstallHook();
}

// Plugin unload cleanup: stop the hook and clear the caches.
void Shutdown() { UninstallHook(); }

std::uint64_t Apply(void* self, void* a1, void* a2, void* a3) {
  (void)a2;
  (void)a3;
  ApplyEntries(self, a1, 1);
  return static_cast<std::uint64_t>(1);
}

std::uint64_t Remove(void* self, void* a1, void* a2, void* a3) {
  (void)a2;
  (void)a3;
  ApplyEntries(self, a1, -1);
  return static_cast<std::uint64_t>(1);
}

// This module's hook install entry: a capture-less lambda (usable as a function pointer), with state
// kept at file scope for the detour and the context callback to share. Returns 0 when the entry is
// unavailable: the effect degrades to no scaling and the whole registration does not fail because of
// it.
const bridge::EffectPrepareFn kCityYieldPrepare = +[](void* /*userData*/) -> int {
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
  g_desc.typeName = "EFFECT_YKKZ000_ADJUST_CITY_YIELD_PER_POPULATION_MODIFIER";
  g_desc.templateEffect = "EFFECT_ADJUST_CITY_YIELD_MODIFIER";
#if !defined(YKKZ000_DISABLE_CUSTOM_BEHAVIOR)
  const bridge::EngineApi* engine = host.engine;
  if (engine == nullptr) {
    return nullptr;
  }
  g_impl.templateApply = engine->effectApply;
  g_impl.templateRemove = engine->effectRemove;
  g_impl.apply = &Apply;
  g_impl.remove = &Remove;
  g_impl.label = "city-yield-per-population";
  g_desc.impl = &g_impl;
  g_desc.prepare = kCityYieldPrepare;
#else
  g_desc.impl = nullptr;    // Custom behavior disabled: degrade to the template behavior
  g_desc.prepare = nullptr; // No hook
#endif
  g_described = true;
  return &g_desc;
}

const EffectModule g_module = {"city-yield-per-population", &Describe, &OnContext, &Shutdown};

} // namespace

const EffectModule* CityYieldModule() { return &g_module; }

} // namespace ykkz000::plugin
