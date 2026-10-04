#include "extra_persistence.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <ykkz000/bridge/host.h>
#include <ykkz000/civ6/common.h>

#include "engine_access.h"

// Custom FAutoVariable persistence.
//
// Reverse-engineering record (GameCore_XP2_FinalRelease.dll, build 0x667C6F5B):
//   * City::Instance::Instance (RVA 0x127DE0) and Unit::Instance::Instance (0x39B140) register many
//     engine FAutoVariable members on the object's own FAutoArchive at instance+0x08. Each member is
//     a {vtable(+0x00), archive(+0x08), value(+0x10)} scalar or
//     {vtable(+0x00), archive(+0x08), data(+0x10), capacity(+0x18), size(+0x20)} int-vector object
//     embedded in the instance; the descriptor vtable for the int-vector type is
//     PTR_FUN_1809C9898 (captured at runtime from City+0x4A0, the m_aYieldModifiers member) and for
//     the scalar int type PTR_FUN_1809C1F08 (captured from Unit+0x4B0, m_iDamage).
//   * Registration helper 0x9953D0(variable, name, archive) appends the variable pointer to the
//     archive's vector and records the name; the engine's own constructors set the purecall vtable
//     before the call and the descriptor vtable after, so the helper never dispatches through the
//     variable.
//   * The registration helper takes an engine name object of 24 bytes {buffer, begin, end}, not an
//     MSVC std::string: the buffer is a NUL-terminated engine-aligned allocation the helper copies
//     during the call, and the engine's own constructors release it with 0x036E20 right after.
//   * The int-vector storage is allocated with the engine's _aligned_malloc (Platform::MallocTemp,
//     RVA 0x990900) so the engine may freely reallocate it on load; the aligned-free wrapper is
//     0x036E20(tag, pointer).
//   * The variable object itself is also allocated with the engine's aligned allocator, because the
//     descriptor destructors release it with _aligned_free; the plugin therefore never frees it.
//
// Consequence: allocating our own variable objects with engine-compatible allocations and
// registering them with the object's archive makes the engine recognize them as members; the
// per-object save/load hooks installed by this layer then invoke their descriptor value methods
// explicitly so the values survive save/load. No engine gameplay logic is replaced.
namespace ykkz000::plugin {
namespace {

// FAutoVariable object layout (scalar and int-vector share the prefix).
constexpr std::size_t kVarVTable = 0x00;
constexpr std::size_t kVarArchive = 0x08;
constexpr std::size_t kVarData = 0x10;     // scalar: the int; vector: data pointer
constexpr std::size_t kVarCapacity = 0x18; // vector capacity
constexpr std::size_t kVarSize = 0x20;     // vector size (read as the count by the engine)
constexpr std::size_t kVarObjectBytes = 0x28;

// Descriptor vtable slots used by the engine's own per-object serializers for each embedded member
// (verified against the City/Unit serializer member calls): the engine calls slot +0x08 while
// loading and slot +0x20 while saving. The plugin reuses the same slots on its custom variables so
// the on-disk representation matches the engine's own member of the same descriptor type.
constexpr std::size_t kVarVTableLoadSlot = 0x08;
constexpr std::size_t kVarVTableSaveSlot = 0x20;

// Descriptor member offsets used to capture the engine's own vtables (self-validating: prefer a
// live engine member over a hard-coded data RVA).
constexpr std::size_t kCityVectorDescriptorOffset = 0x4A0; // m_aYieldModifiers (int vector)
constexpr std::size_t kUnitScalarDescriptorOffset = 0x4B0; // m_iDamage (int)
constexpr std::size_t kArchiveOffset = 0x08;

// Stable AutoVariable names. Once shipped these must never change: the engine archive resolves
// variables by name on load.
constexpr char kCityPercentName[] = "m_aYkkz000YieldPercentPerPopulation";
constexpr char kCityPerSuzerainName[] = "m_aYkkz000YieldPercentPerSuzerain";
constexpr char kUnitStrengthName[] = "m_iYkkz000StrengthPerSuzerain";

using RegisterFn = void (*)(void* variable, void* name, void* archive);
using MallocFn = void* (*)(std::size_t size);
using FreeFn = void (*)(void* tag, void* pointer);
using CityConstructorFn = void* (*)(void* self);
using UnitConstructorFn = void* (*)(void* self);
// Engine per-object serialize/deserialize entry: void*(stream, object).
using ObjectSerializeFn = void* (*)(void* stream, void* object);
// AutoVariable descriptor value method: void(variable, stream).
using VariableSerializeFn = void (*)(void* variable, void* stream);

struct VarHandle {
  std::uint8_t* object = nullptr;
};

struct CityVars {
  VarHandle percent;
  VarHandle per_suzerain;
};

std::mutex g_mutex;
std::unordered_map<void*, CityVars> g_cityVars;
std::unordered_map<void*, VarHandle> g_unitVars;
// Variables detached from a destroyed context. They are released at the next context creation,
// where the previous game's objects and archives are guaranteed to be gone, so the engine can never
// dereference a released variable.
std::vector<CityVars> g_pendingCities;
std::vector<VarHandle> g_pendingUnits;
std::atomic<bool> g_enabled{false};

std::mutex g_hookMutex;
CityConstructorFn g_cityCtorOriginal = nullptr;
UnitConstructorFn g_unitCtorOriginal = nullptr;
void* g_cityCtorTarget = nullptr;
void* g_unitCtorTarget = nullptr;

// Per-object serialization hooks: the engine's own City/Unit save/load entries.
ObjectSerializeFn g_citySerializeSaveOriginal = nullptr;
ObjectSerializeFn g_citySerializeLoadOriginal = nullptr;
ObjectSerializeFn g_unitSerializeSaveOriginal = nullptr;
ObjectSerializeFn g_unitSerializeLoadOriginal = nullptr;
void* g_citySerializeSaveTarget = nullptr;
void* g_citySerializeLoadTarget = nullptr;
void* g_unitSerializeSaveTarget = nullptr;
void* g_unitSerializeLoadTarget = nullptr;

void* g_vectorVTable = nullptr;
void* g_scalarVTable = nullptr;

// Engine name object passed to the archive registration helper: 24 bytes {buffer, begin, end}. The
// buffer is a NUL-terminated engine-aligned allocation; the helper copies the name into the archive
// during the call, so the buffer only has to outlive that call.
struct AutoVarName {
  void* buffer;
  const char* begin;
  const char* end;
};

MallocFn EngineMalloc() {
  const bridge::EngineApi* engine = Context().engine;
  return engine != nullptr ? reinterpret_cast<MallocFn>(engine->engineAlignedMalloc) : nullptr;
}

FreeFn EngineFree() {
  const bridge::EngineApi* engine = Context().engine;
  return engine != nullptr ? reinterpret_cast<FreeFn>(engine->engineAlignedFree) : nullptr;
}

// Drops our lookup record without touching engine memory. The variable object and its data buffer
// are owned by the engine's archive/descriptor destruction path (both release with the engine
// aligned allocator), so freeing them here would double-free. Each record is ~0x28 bytes plus an
// optional 0x40-byte vector buffer, so letting the process reclaim them is negligible.
void ForgetVar(VarHandle) {}

// Registers one custom variable object on an object's archive and returns its handle. On failure
// nothing is registered.
VarHandle CreateVariable(void* archive, const char* name, void* vtable, bool is_vector) {
  VarHandle handle;
  const bridge::EngineApi* engine = Context().engine;
  const MallocFn malloc_fn = EngineMalloc();
  const FreeFn free_fn = EngineFree();
  if (engine == nullptr || engine->autoVariableRegister == nullptr || malloc_fn == nullptr ||
      vtable == nullptr || name == nullptr) {
    static std::atomic<bool> kLoggedUnavailable{false};
    if (!kLoggedUnavailable.exchange(true)) {
      LogError("persistence: CreateVariable unavailable (registration entry or descriptor "
               "missing); values will not persist");
    }
    return handle;
  }
  // Engine-aligned allocation: the descriptor destructor releases the variable with _aligned_free,
  // so operator new/delete would corrupt the heap.
  auto* object = static_cast<std::uint8_t*>(malloc_fn(kVarObjectBytes));
  if (object == nullptr) {
    static std::atomic<bool> kLoggedObject{false};
    if (!kLoggedObject.exchange(true)) {
      LogWarnF("persistence: variable object allocation failed name=%s bytes=%zu", name,
               kVarObjectBytes);
    }
    return handle;
  }
  std::memset(object, 0, kVarObjectBytes);

  void* data = nullptr;
  if (is_vector) {
    const std::size_t bytes =
        static_cast<std::size_t>(civ6::kMaxYields) * sizeof(std::int32_t);
    data = malloc_fn(bytes);
    if (data == nullptr) {
      // The variable object is engine-owned memory that must not be freed here; leaving it
      // unreferenced is safer than releasing it through a mismatched allocator.
      static std::atomic<bool> kLoggedData{false};
      if (!kLoggedData.exchange(true)) {
        LogWarnF("persistence: variable data allocation failed name=%s bytes=%zu", name, bytes);
      }
      return handle;
    }
    std::memset(data, 0, bytes);
  }

  // Engine name object: {buffer, begin, end}. Register while the buffer is alive, then release it
  // with the matching engine free, exactly like the engine's own constructors.
  const std::size_t len = std::strlen(name);
  void* name_buffer = malloc_fn(len + 1);
  if (name_buffer == nullptr) {
    static std::atomic<bool> kLoggedName{false};
    if (!kLoggedName.exchange(true)) {
      LogWarnF("persistence: variable name allocation failed name=%s", name);
    }
    return handle;
  }
  std::memcpy(name_buffer, name, len + 1);
  AutoVarName name_object{name_buffer, static_cast<const char*>(name_buffer),
                          static_cast<const char*>(name_buffer) + len + 1};

  const auto register_variable = reinterpret_cast<RegisterFn>(engine->autoVariableRegister);
  register_variable(object, &name_object, archive);
  if (free_fn != nullptr) {
    free_fn(nullptr, name_buffer);
  }

  *reinterpret_cast<void**>(object + kVarVTable) = vtable;
  *reinterpret_cast<void**>(object + kVarArchive) = archive;
  if (is_vector) {
    *reinterpret_cast<void**>(object + kVarData) = data;
    *reinterpret_cast<std::uint64_t*>(object + kVarCapacity) = civ6::kMaxYields;
    *reinterpret_cast<std::uint64_t*>(object + kVarSize) = civ6::kMaxYields;
  } else {
    *reinterpret_cast<std::int32_t*>(object + kVarData) = 0;
  }
  handle.object = object;
  LogDebugF("persistence: registered name=%s descriptor=%p archive=%p var=%p data=%p "
            "vector=%d entries=%zu",
            name, vtable, archive, object, data, is_vector ? 1 : 0,
            is_vector ? civ6::kMaxYields : 0);
  return handle;
}

void RegisterCity(void* city) {
  const bridge::EngineApi* engine = Context().engine;
  if (engine == nullptr || engine->autoVariableRegister == nullptr || city == nullptr) {
    return;
  }
  void* vector_vtable = nullptr;
  if (!TryReadAt(city, kCityVectorDescriptorOffset, vector_vtable) || vector_vtable == nullptr) {
    static std::atomic<bool> kLoggedMissing{false};
    if (!kLoggedMissing.exchange(true)) {
      LogWarnF("persistence: city int-vector descriptor unavailable at +0x%zX; city values will "
               "not persist",
               kCityVectorDescriptorOffset);
    }
    return;
  }
  void* archive_vtable = nullptr;
  if (!TryReadAt(city, kArchiveOffset, archive_vtable) || archive_vtable == nullptr) {
    static std::atomic<bool> kLoggedMissing{false};
    if (!kLoggedMissing.exchange(true)) {
      LogWarnF("persistence: city archive unavailable at +0x%zX; city values will not persist",
               kArchiveOffset);
    }
    return;
  }
  g_vectorVTable = vector_vtable;
  void* const archive = static_cast<std::uint8_t*>(city) + kArchiveOffset;

  CityVars vars;
  vars.percent = CreateVariable(archive, kCityPercentName, g_vectorVTable, true);
  if (vars.percent.object == nullptr) {
    return;
  }
  vars.per_suzerain = CreateVariable(archive, kCityPerSuzerainName, g_vectorVTable, true);
  if (vars.per_suzerain.object == nullptr) {
    ForgetVar(vars.percent);
    return;
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_cityVars.find(city);
  if (it != g_cityVars.end()) {
    // The address was recycled by a new object whose archive is already a fresh one; the previous
    // archive was destroyed with the old object, so only the lookup record is reset here.
    ForgetVar(it->second.percent);
    ForgetVar(it->second.per_suzerain);
    it->second = vars;
  } else {
    g_cityVars.emplace(city, vars);
  }
#if defined(_DEBUG)
  static std::atomic<long> kRegisterCount{0};
  const long n = ++kRegisterCount;
  if (n <= 8 || (n % 4096) == 0) {
    LogDebugF("persistence: register city=%p archive=%p descriptor=%p percent=%p "
              "per_suzerain=%p",
              city, archive, g_vectorVTable, vars.percent.object, vars.per_suzerain.object);
  }
#endif
}

void RegisterUnit(void* unit) {
  const bridge::EngineApi* engine = Context().engine;
  if (engine == nullptr || engine->autoVariableRegister == nullptr || unit == nullptr) {
    return;
  }
  void* scalar_vtable = nullptr;
  if (!TryReadAt(unit, kUnitScalarDescriptorOffset, scalar_vtable) || scalar_vtable == nullptr) {
    static std::atomic<bool> kLoggedMissing{false};
    if (!kLoggedMissing.exchange(true)) {
      LogWarnF("persistence: unit scalar descriptor unavailable at +0x%zX; unit values will not "
               "persist",
               kUnitScalarDescriptorOffset);
    }
    return;
  }
  void* archive_vtable = nullptr;
  if (!TryReadAt(unit, kArchiveOffset, archive_vtable) || archive_vtable == nullptr) {
    static std::atomic<bool> kLoggedMissing{false};
    if (!kLoggedMissing.exchange(true)) {
      LogWarnF("persistence: unit archive unavailable at +0x%zX; unit values will not persist",
               kArchiveOffset);
    }
    return;
  }
  g_scalarVTable = scalar_vtable;
  void* const archive = static_cast<std::uint8_t*>(unit) + kArchiveOffset;

  VarHandle strength = CreateVariable(archive, kUnitStrengthName, g_scalarVTable, false);
  if (strength.object == nullptr) {
    return;
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_unitVars.find(unit);
  if (it != g_unitVars.end()) {
    // Address recycled by a new unit: only the lookup record is reset (see RegisterCity).
    ForgetVar(it->second);
    it->second = strength;
  } else {
    g_unitVars.emplace(unit, strength);
  }
#if defined(_DEBUG)
  static std::atomic<long> kRegisterCount{0};
  const long n = ++kRegisterCount;
  if (n <= 8 || (n % 4096) == 0) {
    LogDebugF("persistence: register unit=%p archive=%p descriptor=%p var=%p", unit, archive,
              g_scalarVTable, strength.object);
  }
#endif
}

void* CityConstructor_Hook(void* self) {
  if (g_cityCtorOriginal != nullptr) {
    (void)g_cityCtorOriginal(self);
  }
  if (self != nullptr && g_enabled.load(std::memory_order_acquire)) {
    RegisterCity(self);
  }
  // A constructor returns its this pointer; returning self keeps the engine call chain intact.
  return self;
}

void* UnitConstructor_Hook(void* self) {
  if (g_unitCtorOriginal != nullptr) {
    (void)g_unitCtorOriginal(self);
  }
  if (self != nullptr && g_enabled.load(std::memory_order_acquire)) {
    RegisterUnit(self);
  }
  return self;
}

std::size_t VectorEntryCount(void* object) {
  std::uint64_t size = 0;
  std::uint64_t capacity = 0;
  if (!TryReadAt(object, kVarSize, size) || !TryReadAt(object, kVarCapacity, capacity)) {
    return 0;
  }
  // Both orderings ({data, capacity, size} or {data, size, capacity}) are handled by taking the
  // smaller of the two fields: it never exceeds the real allocation, so writes cannot overflow.
  const std::uint64_t bound = size < capacity ? size : capacity;
  if (size > civ6::kMaxYields || capacity > civ6::kMaxYields) {
    static std::atomic<bool> kLoggedOversized{false};
    if (!kLoggedOversized.exchange(true)) {
      LogWarnF("persistence: variable size/capacity exceed kMaxYields size=%llu cap=%llu; "
               "clamping",
               static_cast<unsigned long long>(size),
               static_cast<unsigned long long>(capacity));
    }
  }
  return bound < civ6::kMaxYields ? static_cast<std::size_t>(bound)
                                  : static_cast<std::size_t>(civ6::kMaxYields);
}

void CopyIntoVector(const VarHandle& handle, const std::int32_t* source) {
  if (handle.object == nullptr || source == nullptr) {
    return;
  }
  void* data = nullptr;
  if (!TryReadAt(handle.object, kVarData, data) || data == nullptr) {
    static std::atomic<bool> kLoggedNullData{false};
    if (!kLoggedNullData.exchange(true)) {
      LogWarn("persistence: variable data pointer unavailable; values not written");
    }
    return;
  }
  const std::size_t count = VectorEntryCount(handle.object);
  auto* base = static_cast<std::uint8_t*>(data);
  for (std::size_t i = 0; i < count; ++i) {
    (void)TryWriteAt(base + i * sizeof(std::int32_t), std::size_t{0}, source[i]);
  }
}

// Reads the stored values into out (kMaxYields entries, zeroed first); returns true when at least
// one stored value is non-zero.
bool ReadFromVector(const VarHandle& handle, std::int32_t* out) {
  std::memset(out, 0, static_cast<std::size_t>(civ6::kMaxYields) * sizeof(std::int32_t));
  if (handle.object == nullptr) {
    return false;
  }
  void* data = nullptr;
  if (!TryReadAt(handle.object, kVarData, data) || data == nullptr) {
    static std::atomic<bool> kLoggedNullData{false};
    if (!kLoggedNullData.exchange(true)) {
      LogWarn("persistence: variable data pointer unavailable; no values read");
    }
    return false;
  }
  const std::size_t count = VectorEntryCount(handle.object);
  auto* base = static_cast<const std::uint8_t*>(data);
  bool any = false;
  for (std::size_t i = 0; i < count; ++i) {
    std::int32_t value = 0;
    if (!TryReadAt(base + i * sizeof(std::int32_t), std::size_t{0}, value)) {
      break;
    }
    out[i] = value;
    if (value != 0) {
      any = true;
    }
  }
  return any;
}

CityVars LookupCityVars(void* city) {
  CityVars vars;
  if (!g_enabled.load(std::memory_order_acquire) || city == nullptr) {
    return vars;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_cityVars.find(city);
  return it != g_cityVars.end() ? it->second : vars;
}

VarHandle LookupUnitVar(void* unit) {
  VarHandle handle;
  if (!g_enabled.load(std::memory_order_acquire) || unit == nullptr) {
    return handle;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_unitVars.find(unit);
  return it != g_unitVars.end() ? it->second : handle;
}

// Invokes one descriptor vtable slot of a custom variable (see kVarVTable*Slot). Every read is
// validated, so a stale handle or a missing descriptor method is a silent no-op rather than a crash.
void InvokeVariableMethod(void* object, std::size_t slot, void* stream) {
  if (object == nullptr || stream == nullptr) {
    return;
  }
  void* vtable = nullptr;
  if (!TryReadAt(object, kVarVTable, vtable) || vtable == nullptr) {
    return;
  }
  void* method = nullptr;
  if (!TryReadAt(vtable, slot, method) || method == nullptr) {
    return;
  }
  reinterpret_cast<VariableSerializeFn>(method)(object, stream);
}

// Appends the city's custom variables after the engine's own members on save. The order (percent,
// then per_suzerain) is fixed and must match CitySerializeLoad_Hook exactly.
void SaveCityVariables(void* stream, void* city) {
  const CityVars vars = LookupCityVars(city);
  InvokeVariableMethod(vars.percent.object, kVarVTableSaveSlot, stream);
  InvokeVariableMethod(vars.per_suzerain.object, kVarVTableSaveSlot, stream);
}

// Reads the city's custom variables back after the engine's own members on load. `vars` is captured
// before the original load runs, while the object and its registered variables already exist.
void LoadCityVariables(void* stream, const CityVars& vars) {
  InvokeVariableMethod(vars.percent.object, kVarVTableLoadSlot, stream);
  InvokeVariableMethod(vars.per_suzerain.object, kVarVTableLoadSlot, stream);
}

// Forward the engine's own city save once, then append our values through the descriptor vtable.
void* CitySerializeSave_Hook(void* stream, void* city) {
  void* result =
      g_citySerializeSaveOriginal != nullptr ? g_citySerializeSaveOriginal(stream, city) : nullptr;
  if (g_citySerializeSaveOriginal != nullptr && g_enabled.load(std::memory_order_acquire)) {
    SaveCityVariables(stream, city);
  }
  return result;
}

// Forward the engine's own city load once, then read our values through the descriptor vtable. The
// lookup happens before the original so the variable handles are captured while the map is intact.
void* CitySerializeLoad_Hook(void* stream, void* city) {
  const CityVars vars = LookupCityVars(city);
  void* result =
      g_citySerializeLoadOriginal != nullptr ? g_citySerializeLoadOriginal(stream, city) : nullptr;
  if (g_citySerializeLoadOriginal != nullptr && g_enabled.load(std::memory_order_acquire)) {
    LoadCityVariables(stream, vars);
  }
  return result;
}

// Unit counterpart: a single scalar AutoVariable appended after the engine's own unit members.
void* UnitSerializeSave_Hook(void* stream, void* unit) {
  void* result =
      g_unitSerializeSaveOriginal != nullptr ? g_unitSerializeSaveOriginal(stream, unit) : nullptr;
  if (g_unitSerializeSaveOriginal != nullptr && g_enabled.load(std::memory_order_acquire)) {
    const VarHandle handle = LookupUnitVar(unit);
    InvokeVariableMethod(handle.object, kVarVTableSaveSlot, stream);
  }
  return result;
}

void* UnitSerializeLoad_Hook(void* stream, void* unit) {
  const VarHandle handle = LookupUnitVar(unit);
  void* result =
      g_unitSerializeLoadOriginal != nullptr ? g_unitSerializeLoadOriginal(stream, unit) : nullptr;
  if (g_unitSerializeLoadOriginal != nullptr && g_enabled.load(std::memory_order_acquire)) {
    InvokeVariableMethod(handle.object, kVarVTableLoadSlot, stream);
  }
  return result;
}

// Installs the four per-object serialization hooks (idempotent, all-or-nothing). A partial install
// would append to save without reading on load (or the reverse) and desynchronize the engine stream,
// so any failure removes everything already installed. Called only after EnsurePersistenceHooks has
// enabled the layer, so the constructor hooks that create the variables are already active.
bool EnsureSerializationHooks() {
  const bridge::Host* host = Context().host;
  const bridge::EngineApi* engine = Context().engine;
  if (host == nullptr || host->installHook == nullptr || host->removeHook == nullptr ||
      engine == nullptr) {
    return false;
  }

  std::lock_guard<std::mutex> lock(g_hookMutex);
  if (g_citySerializeSaveTarget != nullptr && g_citySerializeLoadTarget != nullptr &&
      g_unitSerializeSaveTarget != nullptr && g_unitSerializeLoadTarget != nullptr) {
    return true; // Idempotent fast path: already installed.
  }

  struct HookEntry {
    void* target;
    void* detour;
    ObjectSerializeFn* original;
    void** installed;
    const char* name;
  };
  const HookEntry entries[] = {
      {engine->citySerializeSave, reinterpret_cast<void*>(&CitySerializeSave_Hook),
       &g_citySerializeSaveOriginal, &g_citySerializeSaveTarget, "city-serialize-save"},
      {engine->citySerializeLoad, reinterpret_cast<void*>(&CitySerializeLoad_Hook),
       &g_citySerializeLoadOriginal, &g_citySerializeLoadTarget, "city-serialize-load"},
      {engine->unitSerializeSave, reinterpret_cast<void*>(&UnitSerializeSave_Hook),
       &g_unitSerializeSaveOriginal, &g_unitSerializeSaveTarget, "unit-serialize-save"},
      {engine->unitSerializeLoad, reinterpret_cast<void*>(&UnitSerializeLoad_Hook),
       &g_unitSerializeLoadOriginal, &g_unitSerializeLoadTarget, "unit-serialize-load"},
  };

  bool ok = true;
  for (const HookEntry& entry : entries) {
    if (entry.target == nullptr) {
      static std::atomic<bool> kLoggedMissing{false};
      if (!kLoggedMissing.exchange(true)) {
        LogWarn("persistence: serialization entry unavailable; values will not survive save/load");
      }
      ok = false;
      break;
    }
    void* original = nullptr;
    const int status = host->installHook(host->pluginHandle, entry.target, entry.detour, &original);
    if (status != 0 || original == nullptr) {
      LogErrorF("persistence: %s hook install -> %d", entry.name, status);
      ok = false;
      break;
    }
    *entry.original = reinterpret_cast<ObjectSerializeFn>(original);
    *entry.installed = entry.target;
  }

  if (!ok) {
    if (g_citySerializeSaveTarget != nullptr) {
      (void)host->removeHook(host->pluginHandle, g_citySerializeSaveTarget);
    }
    if (g_citySerializeLoadTarget != nullptr) {
      (void)host->removeHook(host->pluginHandle, g_citySerializeLoadTarget);
    }
    if (g_unitSerializeSaveTarget != nullptr) {
      (void)host->removeHook(host->pluginHandle, g_unitSerializeSaveTarget);
    }
    if (g_unitSerializeLoadTarget != nullptr) {
      (void)host->removeHook(host->pluginHandle, g_unitSerializeLoadTarget);
    }
    g_citySerializeSaveTarget = nullptr;
    g_citySerializeLoadTarget = nullptr;
    g_unitSerializeSaveTarget = nullptr;
    g_unitSerializeLoadTarget = nullptr;
    g_citySerializeSaveOriginal = nullptr;
    g_citySerializeLoadOriginal = nullptr;
    g_unitSerializeSaveOriginal = nullptr;
    g_unitSerializeLoadOriginal = nullptr;
    return false;
  }

  LogInfoF("persistence: serialization hooks active (city save=%p load=%p unit save=%p load=%p)",
           g_citySerializeSaveTarget, g_citySerializeLoadTarget, g_unitSerializeSaveTarget,
           g_unitSerializeLoadTarget);
  return true;
}

void RemoveHooks() {
  const bridge::Host* host = Context().host;
  std::lock_guard<std::mutex> lock(g_hookMutex);
  if (host != nullptr && host->removeHook != nullptr) {
    if (g_cityCtorTarget != nullptr) {
      (void)host->removeHook(host->pluginHandle, g_cityCtorTarget);
    }
    if (g_unitCtorTarget != nullptr) {
      (void)host->removeHook(host->pluginHandle, g_unitCtorTarget);
    }
    if (g_citySerializeSaveTarget != nullptr) {
      (void)host->removeHook(host->pluginHandle, g_citySerializeSaveTarget);
    }
    if (g_citySerializeLoadTarget != nullptr) {
      (void)host->removeHook(host->pluginHandle, g_citySerializeLoadTarget);
    }
    if (g_unitSerializeSaveTarget != nullptr) {
      (void)host->removeHook(host->pluginHandle, g_unitSerializeSaveTarget);
    }
    if (g_unitSerializeLoadTarget != nullptr) {
      (void)host->removeHook(host->pluginHandle, g_unitSerializeLoadTarget);
    }
  }
  g_cityCtorTarget = nullptr;
  g_unitCtorTarget = nullptr;
  g_cityCtorOriginal = nullptr;
  g_unitCtorOriginal = nullptr;
  g_citySerializeSaveTarget = nullptr;
  g_citySerializeLoadTarget = nullptr;
  g_unitSerializeSaveTarget = nullptr;
  g_unitSerializeLoadTarget = nullptr;
  g_citySerializeSaveOriginal = nullptr;
  g_citySerializeLoadOriginal = nullptr;
  g_unitSerializeSaveOriginal = nullptr;
  g_unitSerializeLoadOriginal = nullptr;
}

// Drops the variables detached from a previously destroyed context. The variable objects and their
// buffers are owned by the engine's archive/descriptor lifetime, so they are not released here.
void ReleasePendingVariables() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_pendingCities.clear();
  g_pendingUnits.clear();
}

// Detaches every registered variable without releasing it: used at context destruction, where the
// archives may still be referenced by engine shutdown code.
void DetachAllVariables() {
  std::unordered_map<void*, CityVars> cities;
  std::unordered_map<void*, VarHandle> units;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    cities.swap(g_cityVars);
    units.swap(g_unitVars);
    g_pendingCities.reserve(g_pendingCities.size() + cities.size());
    g_pendingUnits.reserve(g_pendingUnits.size() + units.size());
    for (auto& entry : cities) {
      g_pendingCities.push_back(entry.second);
    }
    for (auto& entry : units) {
      g_pendingUnits.push_back(entry.second);
    }
  }
}

void DropMapsWithoutFreeing() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_cityVars.clear();
  g_unitVars.clear();
  g_pendingCities.clear();
  g_pendingUnits.clear();
}

// Emergency switch: setting YKKZ000_DISABLE_PERSISTENCE=1 keeps the whole persistence layer off, so
// a crash in variable registration can be bypassed without a rebuild.
bool PersistenceDisabledByEnvironment() {
  const char* value = std::getenv("YKKZ000_DISABLE_PERSISTENCE");
  return value != nullptr && std::strcmp(value, "1") == 0;
}

} // namespace

bool EnsurePersistenceHooks() {
  if (PersistenceDisabledByEnvironment()) {
    static std::atomic<bool> kLoggedDisabled{false};
    if (!kLoggedDisabled.exchange(true)) {
      LogWarn("persistence: disabled by YKKZ000_DISABLE_PERSISTENCE; values will not survive "
             "save/load");
    }
    g_enabled.store(false, std::memory_order_release);
    return false;
  }

  const bridge::Host* host = Context().host;
  const bridge::EngineApi* engine = Context().engine;
  if (host == nullptr || host->installHook == nullptr || host->removeHook == nullptr ||
      engine == nullptr || engine->cityConstructor == nullptr ||
      engine->unitConstructor == nullptr || engine->autoVariableRegister == nullptr ||
      engine->engineAlignedMalloc == nullptr || engine->engineAlignedFree == nullptr) {
    static std::atomic<bool> kLoggedUnavailable{false};
    if (!kLoggedUnavailable.exchange(true)) {
      LogWarn("persistence: engine AutoVariable entries unavailable; values will not survive "
             "save/load");
    }
    return false;
  }

  // The previous context's variables are safe to release now (its objects and archives are gone).
  ReleasePendingVariables();

  // Install the constructor hooks under the hook lock, then release it before installing the
  // serialization hooks (which take the same lock) so the two install phases cannot deadlock.
  bool ctor_ok = false;
  {
    std::lock_guard<std::mutex> lock(g_hookMutex);
    const bool city_installed =
        g_cityCtorTarget != nullptr && g_cityCtorOriginal != nullptr;
    if (!city_installed) {
      void* original = nullptr;
      const int status = host->installHook(host->pluginHandle, engine->cityConstructor,
                                           reinterpret_cast<void*>(&CityConstructor_Hook),
                                           &original);
      if (status == 0) {
        g_cityCtorTarget = engine->cityConstructor;
        g_cityCtorOriginal = reinterpret_cast<CityConstructorFn>(original);
      } else {
        (void)host->removeHook(host->pluginHandle, engine->cityConstructor);
        LogErrorF("persistence: city constructor hook install -> %d", status);
      }
    }
    const bool unit_installed =
        g_unitCtorTarget != nullptr && g_unitCtorOriginal != nullptr;
    if (!unit_installed) {
      void* original = nullptr;
      const int status = host->installHook(host->pluginHandle, engine->unitConstructor,
                                           reinterpret_cast<void*>(&UnitConstructor_Hook),
                                           &original);
      if (status == 0) {
        g_unitCtorTarget = engine->unitConstructor;
        g_unitCtorOriginal = reinterpret_cast<UnitConstructorFn>(original);
      } else {
        (void)host->removeHook(host->pluginHandle, engine->unitConstructor);
        LogErrorF("persistence: unit constructor hook install -> %d", status);
      }
    }

    if (g_cityCtorOriginal == nullptr || g_unitCtorOriginal == nullptr) {
      // Partial install: undo so the plugin does not keep a half-active persistence layer.
      if (g_cityCtorOriginal != nullptr) {
        (void)host->removeHook(host->pluginHandle, g_cityCtorTarget);
      }
      if (g_unitCtorOriginal != nullptr) {
        (void)host->removeHook(host->pluginHandle, g_unitCtorTarget);
      }
      g_cityCtorTarget = nullptr;
      g_cityCtorOriginal = nullptr;
      g_unitCtorTarget = nullptr;
      g_unitCtorOriginal = nullptr;
    } else {
      ctor_ok = true;
    }
  }
  if (!ctor_ok) {
    return false;
  }

  g_enabled.store(true, std::memory_order_release);
  // The serialization hooks serialize the variables the constructor hooks register, so they are
  // installed only after the layer is enabled.
  (void)EnsureSerializationHooks();
  LogInfoF("persistence: active; constructor hooks installed (city=%p unit=%p) serialization=%d",
           g_cityCtorTarget, g_unitCtorTarget,
           g_citySerializeSaveTarget != nullptr && g_citySerializeLoadTarget != nullptr &&
                   g_unitSerializeSaveTarget != nullptr && g_unitSerializeLoadTarget != nullptr
               ? 1
               : 0);
  return true;
}

void ShutdownPersistence() {
  g_enabled.store(false, std::memory_order_release);
  RemoveHooks();
  // Detach rather than release: engine shutdown may still walk the archives. The variables are
  // released at the next context creation (see EnsurePersistenceHooks).
  DetachAllVariables();
}

void ResetPersistenceForUnload() {
  g_enabled.store(false, std::memory_order_release);
  RemoveHooks();
  DropMapsWithoutFreeing();
}

void PersistCityValues(void* city, const std::int32_t* percent,
                       const std::int32_t* per_suzerain) {
  const CityVars vars = LookupCityVars(city);
  CopyIntoVector(vars.percent, percent);
  CopyIntoVector(vars.per_suzerain, per_suzerain);
}

bool LoadCityValues(void* city, std::int32_t* percent_out, std::int32_t* per_suzerain_out) {
  if (percent_out == nullptr || per_suzerain_out == nullptr) {
    return false;
  }
  const CityVars vars = LookupCityVars(city);
  const bool percent = ReadFromVector(vars.percent, percent_out);
  const bool per_suzerain = ReadFromVector(vars.per_suzerain, per_suzerain_out);
  if (percent || per_suzerain) {
    static std::atomic<long> kRestoreCount{0};
    const long restore_n = ++kRestoreCount;
    if (restore_n <= 8 || (restore_n % 4096) == 0) {
      LogInfoF("persistence: restored city=%p pct[1]=%d suz[1]=%d", city, percent_out[1],
               per_suzerain_out[1]);
    }
  }
  return percent || per_suzerain;
}

void PersistUnitStrength(void* unit, std::int32_t value) {
  const VarHandle handle = LookupUnitVar(unit);
  if (handle.object == nullptr) {
    return;
  }
  (void)TryWriteAt(handle.object, kVarData, value);
#if defined(_DEBUG)
  static std::atomic<long> kPersistCount{0};
  const long n = ++kPersistCount;
  if (n <= 8 || (n % 4096) == 0) {
    LogDebugF("persistence: persist unit=%p strength=%d var=%p", unit, value, handle.object);
  }
#endif
}

bool LoadUnitStrength(void* unit, std::int32_t& out) {
  out = 0;
  const VarHandle handle = LookupUnitVar(unit);
  if (handle.object == nullptr) {
    return false;
  }
  std::int32_t value = 0;
  if (!TryReadAt(handle.object, kVarData, value)) {
    return false;
  }
  out = value;
#if defined(_DEBUG)
  static std::atomic<long> kLoadCount{0};
  const long n = ++kLoadCount;
  if (n <= 8 || (n % 4096) == 0) {
    LogDebugF("persistence: load unit=%p strength=%d var=%p", unit, value, handle.object);
  }
#endif
  if (value != 0) {
    static std::atomic<long> kRestoreCount{0};
    const long restore_n = ++kRestoreCount;
    if (restore_n <= 8 || (restore_n % 4096) == 0) {
      LogInfoF("persistence: restored unit=%p strength=%d", unit, value);
    }
  }
  return value != 0;
}

} // namespace ykkz000::plugin
