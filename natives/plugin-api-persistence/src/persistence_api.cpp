#include <ykkz000/plugin/persistence_api.h>

#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <ykkz000/bridge/host.h>
#include <ykkz000/bridge/log.h>
#include <ykkz000/export.h>

// Variable-description-driven AutoVariable persistence for engine City objects, plus City/Unit
// lifecycle notifications.
//
// Reverse-engineering record (GameCore_XP2_FinalRelease.dll, build 0x667C6F5B):
//   * City::Instance::Instance (RVA 0x127DE0) registers many engine FAutoVariable members on the
//     object's own FAutoArchive at instance+0x08. Each int-vector member is
//     {vtable(+0x00), archive(+0x08), data(+0x10), capacity(+0x18), size(+0x20)} embedded in the
//     instance; the descriptor vtable for the int-vector type is captured at runtime from
//     City+0x4A0 (the m_aYieldModifiers member).
//   * Registration helper 0x9953D0(variable, name, archive) appends the variable pointer to the
//     archive's vector and records the name; the engine name object is 24 bytes
//     {buffer, begin, end} whose buffer the helper copies during the call.
//   * The int-vector storage is allocated with the engine's aligned allocator (Platform::MallocTemp,
//     RVA 0x990900) so the engine may reallocate it on load; the aligned-free wrapper is
//     0x036E20(tag, pointer).
//   * Per-object save/load (EngineApi::citySerializeSave/Load) is hooked so the custom variables are
//     appended after the engine's own members on save and read back in the same order on load.
//
// The variable names and capacities are supplied by consumers through declare_city_int_vector; this
// plugin holds no city-yield-specific knowledge.
namespace ykkz000::plugin {
namespace {

const bridge::Host* g_host = nullptr;
const bridge::EngineApi* g_engine = nullptr;

// Captures the host handed to GetPlugin (called by the plugin entry points).
void BindHost(const bridge::Host* host) {
  g_host = host;
  g_engine = host != nullptr ? host->engine : nullptr;
}

void Log(int level, const char* message) {
  if (g_host != nullptr && g_host->log != nullptr) {
    g_host->log(level, message);
  }
}

void LogF(int level, const char* format, ...) {
  char buffer[1024] = {};
  va_list args;
  va_start(args, format);
  _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
  va_end(args);
  Log(level, buffer);
}

// FAutoVariable object layout.
constexpr std::size_t kVarVTable = 0x00;
constexpr std::size_t kVarArchive = 0x08;
constexpr std::size_t kVarData = 0x10;     // vector: data pointer
constexpr std::size_t kVarCapacity = 0x18; // vector capacity
constexpr std::size_t kVarSize = 0x20;     // vector size (read as the count by the engine)
constexpr std::size_t kVarObjectBytes = 0x28;

// Descriptor vtable slots used by the engine's own per-object serializers for each embedded member:
// the engine calls slot +0x08 while loading and slot +0x20 while saving.
constexpr std::size_t kVarVTableLoadSlot = 0x08;
constexpr std::size_t kVarVTableSaveSlot = 0x20;

// Descriptor member offsets used to capture the engine's own int-vector vtable (self-validating:
// prefer a live engine member over a hard-coded data RVA).
constexpr std::size_t kCityVectorDescriptorOffset = 0x4A0; // m_aYieldModifiers (int vector)
constexpr std::size_t kArchiveOffset = 0x08;

using RegisterFn = void (*)(void* variable, void* name, void* archive);
using MallocFn = void* (*)(std::size_t size);
using FreeFn = void (*)(void* tag, void* pointer);
using CityConstructorFn = void* (*)(void* self);
using UnitConstructorFn = void* (*)(void* self);
using ObjectDestructorFn = void (*)(void* object);
// Engine per-object serialize/deserialize entry: void*(stream, object).
using ObjectSerializeFn = void* (*)(void* stream, void* object);
// AutoVariable descriptor value method: void(variable, stream).
using VariableSerializeFn = void (*)(void* variable, void* stream);

// -- Descriptors (declaration order defines the on-disk payload order) --
struct Descriptor {
  std::string name;
  std::uint32_t capacity = 0;
};
std::mutex g_descMutex;
std::vector<Descriptor> g_descriptors;
std::unordered_map<std::string, std::size_t> g_descriptorIndex;

// -- Per-city variable handles (aligned with g_descriptors by index) --
struct VarHandle {
  std::uint8_t* object = nullptr;
};
std::mutex g_mutex;
std::unordered_map<void*, std::vector<VarHandle>> g_cityVars;
// Variables detached from a destroyed context. They are released at the next context creation,
// where the previous game's objects and archives are guaranteed to be gone.
std::vector<std::vector<VarHandle>> g_pendingCities;
std::atomic<bool> g_enabled{false};   // Persistence (constructor + serialization) is active
std::atomic<bool> g_lifecycle{false}; // City/Unit lifecycle notifications are active

// Whether the resident hooks were installed. They are installed once (from GetPlugin) and stay
// installed for the whole process; contexts only toggle the gates above. A gate is never turned on
// for a hook set that was not installed, and the flags are cleared only when DestroyPlugin
// uninstalls the hooks.
std::atomic<bool> g_lifecycleHooksReady{false};
std::atomic<bool> g_serializationHooksReady{false};

// -- Lifecycle subscribers --
struct Subscriber {
  void* user = nullptr;
  ObjectLifecycleFn city_created = nullptr;
  ObjectLifecycleFn city_destroyed = nullptr;
  ObjectLifecycleFn unit_created = nullptr;
  ObjectLifecycleFn unit_destroyed = nullptr;
};
std::mutex g_lifecycleMutex;
std::vector<Subscriber> g_subscribers;

// -- Hook state --
std::mutex g_hookMutex;
CityConstructorFn g_cityCtorOriginal = nullptr;
void* g_cityCtorTarget = nullptr;
UnitConstructorFn g_unitCtorOriginal = nullptr;
void* g_unitCtorTarget = nullptr;
ObjectDestructorFn g_cityDtorOriginal = nullptr;
ObjectDestructorFn g_unitDtorOriginal = nullptr;
void* g_cityDtorTarget = nullptr;
void* g_unitDtorTarget = nullptr;
ObjectSerializeFn g_citySerializeSaveOriginal = nullptr;
ObjectSerializeFn g_citySerializeLoadOriginal = nullptr;
void* g_citySerializeSaveTarget = nullptr;
void* g_citySerializeLoadTarget = nullptr;

void* g_vectorVTable = nullptr;

// Engine name object passed to the archive registration helper: 24 bytes {buffer, begin, end}.
struct AutoVarName {
  void* buffer;
  const char* begin;
  const char* end;
};

MallocFn EngineMalloc() {
  return g_engine != nullptr ? reinterpret_cast<MallocFn>(g_engine->engineAlignedMalloc) : nullptr;
}

FreeFn EngineFree() {
  return g_engine != nullptr ? reinterpret_cast<FreeFn>(g_engine->engineAlignedFree) : nullptr;
}

template <typename T>
bool TryReadAt(const void* base, std::size_t offset, T& out) {
  if (g_host == nullptr || g_host->readField == nullptr) {
    return false;
  }
  return g_host->readField(base, offset, sizeof(T), &out) != 0;
}

template <typename T>
bool TryWriteAt(void* base, std::size_t offset, const T& value) {
  if (g_host == nullptr || g_host->writeField == nullptr) {
    return false;
  }
  return g_host->writeField(base, offset, sizeof(T), &value) != 0;
}

void DispatchCityCreated(void* city) {
  std::vector<Subscriber> subscribers;
  {
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    subscribers = g_subscribers;
  }
  for (const Subscriber& subscriber : subscribers) {
    if (subscriber.city_created != nullptr) {
      subscriber.city_created(subscriber.user, city);
    }
  }
}

void DispatchCityDestroyed(void* city) {
  std::vector<Subscriber> subscribers;
  {
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    subscribers = g_subscribers;
  }
  for (const Subscriber& subscriber : subscribers) {
    if (subscriber.city_destroyed != nullptr) {
      subscriber.city_destroyed(subscriber.user, city);
    }
  }
}

void DispatchUnitCreated(void* unit) {
  std::vector<Subscriber> subscribers;
  {
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    subscribers = g_subscribers;
  }
  for (const Subscriber& subscriber : subscribers) {
    if (subscriber.unit_created != nullptr) {
      subscriber.unit_created(subscriber.user, unit);
    }
  }
}

void DispatchUnitDestroyed(void* unit) {
  std::vector<Subscriber> subscribers;
  {
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    subscribers = g_subscribers;
  }
  for (const Subscriber& subscriber : subscribers) {
    if (subscriber.unit_destroyed != nullptr) {
      subscriber.unit_destroyed(subscriber.user, unit);
    }
  }
}

// Registers one custom int-vector variable object on an object's archive; returns its handle.
VarHandle CreateVariable(void* archive, const char* name, void* vtable, std::uint32_t capacity) {
  VarHandle handle;
  const MallocFn malloc_fn = EngineMalloc();
  const FreeFn free_fn = EngineFree();
  if (g_engine == nullptr || g_engine->autoVariableRegister == nullptr || malloc_fn == nullptr ||
      vtable == nullptr || name == nullptr || capacity == 0) {
    static std::atomic<bool> kLoggedUnavailable{false};
    if (!kLoggedUnavailable.exchange(true)) {
      Log(static_cast<int>(bridge::LogLevel::kError),
          "persistence: CreateVariable unavailable (registration entry or descriptor missing)");
    }
    return handle;
  }
  auto* object = static_cast<std::uint8_t*>(malloc_fn(kVarObjectBytes));
  if (object == nullptr) {
    static std::atomic<bool> kLoggedObject{false};
    if (!kLoggedObject.exchange(true)) {
      LogF(static_cast<int>(bridge::LogLevel::kWarning),
           "persistence: variable object allocation failed name=%s bytes=%zu", name,
           kVarObjectBytes);
    }
    return handle;
  }
  std::memset(object, 0, kVarObjectBytes);

  const std::size_t bytes = static_cast<std::size_t>(capacity) * sizeof(std::int32_t);
  void* data = malloc_fn(bytes);
  if (data == nullptr) {
    static std::atomic<bool> kLoggedData{false};
    if (!kLoggedData.exchange(true)) {
      LogF(static_cast<int>(bridge::LogLevel::kWarning),
           "persistence: variable data allocation failed name=%s bytes=%zu", name, bytes);
    }
    return handle;
  }
  std::memset(data, 0, bytes);

  const std::size_t len = std::strlen(name);
  void* name_buffer = malloc_fn(len + 1);
  if (name_buffer == nullptr) {
    static std::atomic<bool> kLoggedName{false};
    if (!kLoggedName.exchange(true)) {
      LogF(static_cast<int>(bridge::LogLevel::kWarning),
           "persistence: variable name allocation failed name=%s", name);
    }
    return handle;
  }
  std::memcpy(name_buffer, name, len + 1);
  AutoVarName name_object{name_buffer, static_cast<const char*>(name_buffer),
                          static_cast<const char*>(name_buffer) + len + 1};

  const auto register_variable = reinterpret_cast<RegisterFn>(g_engine->autoVariableRegister);
  register_variable(object, &name_object, archive);
  if (free_fn != nullptr) {
    free_fn(nullptr, name_buffer);
  }

  *reinterpret_cast<void**>(object + kVarVTable) = vtable;
  *reinterpret_cast<void**>(object + kVarArchive) = archive;
  *reinterpret_cast<void**>(object + kVarData) = data;
  *reinterpret_cast<std::uint64_t*>(object + kVarCapacity) = capacity;
  *reinterpret_cast<std::uint64_t*>(object + kVarSize) = capacity;
  handle.object = object;
  return handle;
}

// Disables persistence for the rest of this game context when the invariant cannot be kept (an
// engine entry or the variable storage became unavailable). The save and the load side observe the
// same gate, so a partial declaration set is never written on one side and read on the other.
void DisablePersistence(const char* reason) {
  g_enabled.store(false, std::memory_order_release);
  static std::atomic<bool> kLoggedDisabled{false};
  if (!kLoggedDisabled.exchange(true)) {
    LogF(static_cast<int>(bridge::LogLevel::kError),
         "persistence: disabled for this context: %s", reason != nullptr ? reason : "unknown");
  }
}

// Number of currently declared variables (the serialization payload length per object).
std::size_t DescriptorCount() {
  std::lock_guard<std::mutex> lock(g_descMutex);
  return g_descriptors.size();
}

// Ensures the city's handle vector covers every declared variable, index-aligned with g_descriptors
// (handles[i] <-> g_descriptors[i]). A city registered for an older declaration set is extended in
// place, never reordered. Returns true when the city holds a complete, fully created set; false when
// persistence is off, nothing is declared, or any variable could not be created. In the failure case
// persistence is disabled for this context so the save and load streams cannot diverge (the plan's
// symmetric "creation failure is fatal" policy; it replaces a short/zero-length stream with turning
// the whole context's custom persistence off).
bool EnsureCityComplete(void* city) {
  if (city == nullptr || !g_enabled.load(std::memory_order_acquire)) {
    return false;
  }
  std::vector<Descriptor> descriptors;
  {
    std::lock_guard<std::mutex> lock(g_descMutex);
    descriptors = g_descriptors;
  }
  const std::size_t count = descriptors.size();
  if (count == 0) {
    return false; // No variable declared: there is nothing to serialize.
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_cityVars.find(city);
  if (it != g_cityVars.end() && it->second.size() == count) {
    bool complete = true;
    for (const VarHandle& handle : it->second) {
      if (handle.object == nullptr) {
        complete = false;
        break;
      }
    }
    if (complete) {
      return true;
    }
    // A previous partial creation left placeholders: fall through and try to fill them.
  }

  // Resolve the descriptor vtable (prefer an already-created variable's captured vtable, otherwise
  // read the engine member at City+0x4A0) and validate the embedded archive at City+0x08.
  const std::vector<VarHandle>* existing = it != g_cityVars.end() ? &it->second : nullptr;
  void* vector_vtable = nullptr;
  if (existing != nullptr) {
    for (const VarHandle& handle : *existing) {
      if (handle.object != nullptr && TryReadAt(handle.object, kVarVTable, vector_vtable) &&
          vector_vtable != nullptr) {
        break;
      }
    }
  }
  void* archive_vtable = nullptr;
  if (vector_vtable == nullptr) {
    (void)TryReadAt(city, kCityVectorDescriptorOffset, vector_vtable);
  }
  if (vector_vtable == nullptr || !TryReadAt(city, kArchiveOffset, archive_vtable) ||
      archive_vtable == nullptr) {
    DisablePersistence("city int-vector descriptor or archive unavailable");
    return false;
  }
  g_vectorVTable = vector_vtable;
  void* const archive = static_cast<std::uint8_t*>(city) + kArchiveOffset;

  std::vector<VarHandle> handles;
  if (existing != nullptr) {
    handles = *existing; // Existing entries keep their index; new descriptor slots are null.
  }
  handles.resize(count);

  bool complete = true;
  for (std::size_t i = 0; i < count; ++i) {
    if (handles[i].object != nullptr) {
      continue;
    }
    handles[i] = CreateVariable(archive, descriptors[i].name.c_str(), vector_vtable,
                                descriptors[i].capacity);
    if (handles[i].object == nullptr) {
      complete = false;
    }
  }

  if (it != g_cityVars.end()) {
    it->second = std::move(handles);
  } else {
    g_cityVars.emplace(city, std::move(handles));
  }

  if (!complete) {
    DisablePersistence("variable creation failed");
    return false;
  }
  return true;
}

// Fresh object construction: drop any record left by a destroyed object that reused this address
// (the destructor hook normally erases it, but a missed teardown must not hand the new object the
// previous archive's variables), then register the complete set on the new archive.
void RegisterCityFresh(void* city) {
  if (city == nullptr || !g_enabled.load(std::memory_order_acquire)) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cityVars.erase(city);
  }
  (void)EnsureCityComplete(city);
}

std::vector<VarHandle> LookupCityVars(void* city) {
  if (!g_enabled.load(std::memory_order_acquire) || city == nullptr) {
    return {};
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_cityVars.find(city);
  return it != g_cityVars.end() ? it->second : std::vector<VarHandle>{};
}

// The complete, index-aligned handle set used for serialization: empty unless the city holds exactly
// `expected` handles and every handle was created. The caller runs EnsureCityComplete first, so this
// only returns a payload set that is symmetric on the save and the load side.
std::vector<VarHandle> CompleteCityVars(void* city, std::size_t expected) {
  if (city == nullptr || expected == 0 || !g_enabled.load(std::memory_order_acquire)) {
    return {};
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_cityVars.find(city);
  if (it == g_cityVars.end() || it->second.size() != expected) {
    return {};
  }
  for (const VarHandle& handle : it->second) {
    if (handle.object == nullptr) {
      return {};
    }
  }
  return it->second;
}

std::size_t DescriptorIndexForName(const char* name) {
  if (name == nullptr) {
    return static_cast<std::size_t>(-1);
  }
  std::lock_guard<std::mutex> lock(g_descMutex);
  const auto it = g_descriptorIndex.find(name);
  return it != g_descriptorIndex.end() ? it->second : static_cast<std::size_t>(-1);
}

std::size_t VectorEntryCount(void* object, std::uint32_t capacity) {
  std::uint64_t size = 0;
  std::uint64_t stored_capacity = 0;
  if (!TryReadAt(object, kVarSize, size) || !TryReadAt(object, kVarCapacity, stored_capacity)) {
    return 0;
  }
  const std::uint64_t bound = size < stored_capacity ? size : stored_capacity;
  return bound < capacity ? static_cast<std::size_t>(bound) : static_cast<std::size_t>(capacity);
}

// Invokes one descriptor vtable slot of a custom variable (see kVarVTable*Slot).
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

// Appends the city's complete custom-variable set after the engine's own members on save: exactly
// one payload per declared variable, in declaration order (handles[i] <-> g_descriptors[i]). The
// caller passes CompleteCityVars, so the set is all-or-nothing and must match LoadCityVariables.
void SaveCityVariables(void* stream, const std::vector<VarHandle>& vars) {
  for (const VarHandle& handle : vars) {
    InvokeVariableMethod(handle.object, kVarVTableSaveSlot, stream);
  }
}

// Reads the same complete, index-aligned payload set on load; the read mirror of SaveCityVariables.
void LoadCityVariables(void* stream, const std::vector<VarHandle>& vars) {
  for (const VarHandle& handle : vars) {
    InvokeVariableMethod(handle.object, kVarVTableLoadSlot, stream);
  }
}

// -- Hooks --

void* CityConstructor_Hook(void* self) {
  if (g_cityCtorOriginal != nullptr) {
    (void)g_cityCtorOriginal(self);
  }
  if (self != nullptr && g_enabled.load(std::memory_order_acquire)) {
    RegisterCityFresh(self);
  }
  if (self != nullptr && g_lifecycle.load(std::memory_order_acquire)) {
    DispatchCityCreated(self);
  }
  return self;
}

void* UnitConstructor_Hook(void* self) {
  void* result = self;
  if (g_unitCtorOriginal != nullptr) {
    result = g_unitCtorOriginal(self);
  }
  if (self != nullptr && g_lifecycle.load(std::memory_order_acquire)) {
    DispatchUnitCreated(self);
  }
  return result;
}

void CityDestructor_Hook(void* city) {
  const bool persistence_active = city != nullptr && g_enabled.load(std::memory_order_acquire);
  const bool lifecycle_active = city != nullptr && g_lifecycle.load(std::memory_order_acquire);
  if (g_cityDtorOriginal != nullptr) {
    g_cityDtorOriginal(city);
  }
  if (persistence_active) {
    // Only the lookup record is dropped. The variable objects and their buffers are owned by the
    // engine's archive/descriptor destruction path, so this must not free or dereference them.
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cityVars.erase(city);
  }
  if (lifecycle_active) {
    DispatchCityDestroyed(city);
  }
}

void UnitDestructor_Hook(void* unit) {
  const bool lifecycle_active = unit != nullptr && g_lifecycle.load(std::memory_order_acquire);
  if (g_unitDtorOriginal != nullptr) {
    g_unitDtorOriginal(unit);
  }
  if (lifecycle_active) {
    DispatchUnitDestroyed(unit);
  }
}

void* CitySerializeSave_Hook(void* stream, void* city) {
  const std::size_t expected = DescriptorCount();
  const bool active = expected > 0 && g_enabled.load(std::memory_order_acquire) && city != nullptr;
  if (active) {
    (void)EnsureCityComplete(city);
  }
  void* result =
      g_citySerializeSaveOriginal != nullptr ? g_citySerializeSaveOriginal(stream, city) : nullptr;
  if (g_citySerializeSaveOriginal != nullptr && expected > 0 &&
      g_enabled.load(std::memory_order_acquire)) {
    SaveCityVariables(stream, CompleteCityVars(city, expected));
  }
  return result;
}

void* CitySerializeLoad_Hook(void* stream, void* city) {
  const std::size_t expected = DescriptorCount();
  const bool active = expected > 0 && g_enabled.load(std::memory_order_acquire) && city != nullptr;
  if (active) {
    (void)EnsureCityComplete(city);
  }
  const std::vector<VarHandle> vars = CompleteCityVars(city, expected);
  void* result =
      g_citySerializeLoadOriginal != nullptr ? g_citySerializeLoadOriginal(stream, city) : nullptr;
  if (g_citySerializeLoadOriginal != nullptr && expected > 0 &&
      g_enabled.load(std::memory_order_acquire)) {
    LoadCityVariables(stream, vars);
  }
  return result;
}

// Physically removes the resident hooks, clearing the trampoline/target pointers. Called only when
// the plugin is unloaded (DestroyPlugin). Between game contexts the hooks are kept installed and
// only the data gates are switched (SetContextActive), so a repeated context creation reuses the
// existing trampolines instead of reinstalling them.
void UninstallHooks() {
  std::lock_guard<std::mutex> lock(g_hookMutex);
  // Stop the detours from acting before they are physically removed.
  g_enabled.store(false, std::memory_order_release);
  g_lifecycle.store(false, std::memory_order_release);
  g_lifecycleHooksReady.store(false, std::memory_order_release);
  g_serializationHooksReady.store(false, std::memory_order_release);
  if (g_host != nullptr && g_host->removeHook != nullptr) {
    if (g_cityCtorTarget != nullptr) {
      (void)g_host->removeHook(g_host->pluginHandle, g_cityCtorTarget);
    }
    if (g_unitCtorTarget != nullptr) {
      (void)g_host->removeHook(g_host->pluginHandle, g_unitCtorTarget);
    }
    if (g_cityDtorTarget != nullptr) {
      (void)g_host->removeHook(g_host->pluginHandle, g_cityDtorTarget);
    }
    if (g_unitDtorTarget != nullptr) {
      (void)g_host->removeHook(g_host->pluginHandle, g_unitDtorTarget);
    }
    if (g_citySerializeSaveTarget != nullptr) {
      (void)g_host->removeHook(g_host->pluginHandle, g_citySerializeSaveTarget);
    }
    if (g_citySerializeLoadTarget != nullptr) {
      (void)g_host->removeHook(g_host->pluginHandle, g_citySerializeLoadTarget);
    }
  }
  g_cityCtorTarget = nullptr;
  g_cityCtorOriginal = nullptr;
  g_unitCtorTarget = nullptr;
  g_unitCtorOriginal = nullptr;
  g_cityDtorTarget = nullptr;
  g_cityDtorOriginal = nullptr;
  g_unitDtorTarget = nullptr;
  g_unitDtorOriginal = nullptr;
  g_citySerializeSaveTarget = nullptr;
  g_citySerializeLoadTarget = nullptr;
  g_citySerializeSaveOriginal = nullptr;
  g_citySerializeLoadOriginal = nullptr;
}

// Defined below (with the context data-table helpers); declared here because SetContextActive is
// reached from the context callback above them.
void ReleasePendingVariables();
void DetachAllVariables();

// Switches the per-context data gates. The resident detours read these gates at entry, so a
// destroyed context makes them pass straight through the original functions without touching freed
// objects, while the hooks and their trampolines stay installed for the next context.
//   * kCreated: release the previous game's detached variables, then enable the gates according to
//     what was installed and whether any variable was declared.
//   * kDestroyed: disable the gates and detach every registered variable (the engine shutdown may
//     still walk the archives, so the variables are not released here).
void SetContextActive(bool active) {
  if (active) {
    ReleasePendingVariables();
    const bool declared = DescriptorCount() > 0;
    const bool serialization =
        g_serializationHooksReady.load(std::memory_order_acquire) && declared;
    const bool lifecycle = g_lifecycleHooksReady.load(std::memory_order_acquire);
    g_enabled.store(serialization, std::memory_order_release);
    g_lifecycle.store(lifecycle, std::memory_order_release);
    LogF(static_cast<int>(bridge::LogLevel::kInfo),
         "persistence: active (lifecycle=%d serialization=%d)", lifecycle ? 1 : 0,
         serialization ? 1 : 0);
  } else {
    g_enabled.store(false, std::memory_order_release);
    g_lifecycle.store(false, std::memory_order_release);
    DetachAllVariables();
    Log(static_cast<int>(bridge::LogLevel::kInfo),
        "persistence: inactive (context destroyed)");
  }
}

// Installs the two per-object serialization hooks (idempotent, all-or-nothing).
bool EnsureSerializationHooks() {
  if (g_host == nullptr || g_host->installHook == nullptr || g_host->removeHook == nullptr ||
      g_engine == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_hookMutex);
  if (g_citySerializeSaveTarget != nullptr && g_citySerializeLoadTarget != nullptr) {
    return true; // Already installed: reuse; the per-context gate is managed by SetContextActive.
  }
  struct HookEntry {
    void* target;
    void* detour;
    ObjectSerializeFn* original;
    void** installed;
    const char* name;
  };
  const HookEntry entries[] = {
      {g_engine->citySerializeSave, reinterpret_cast<void*>(&CitySerializeSave_Hook),
       &g_citySerializeSaveOriginal, &g_citySerializeSaveTarget, "city-serialize-save"},
      {g_engine->citySerializeLoad, reinterpret_cast<void*>(&CitySerializeLoad_Hook),
       &g_citySerializeLoadOriginal, &g_citySerializeLoadTarget, "city-serialize-load"},
  };
  bool ok = true;
  for (const HookEntry& entry : entries) {
    if (entry.target == nullptr) {
      static std::atomic<bool> kLoggedMissing{false};
      if (!kLoggedMissing.exchange(true)) {
        Log(static_cast<int>(bridge::LogLevel::kWarning),
            "persistence: serialization entry unavailable; values will not survive save/load");
      }
      ok = false;
      break;
    }
    void* original = nullptr;
    const int status = g_host->installHook(g_host->pluginHandle, entry.target, entry.detour, &original);
    if (status != 0 || original == nullptr) {
      LogF(static_cast<int>(bridge::LogLevel::kError), "persistence: %s hook install -> %d",
           entry.name, status);
      ok = false;
      break;
    }
    *entry.original = reinterpret_cast<ObjectSerializeFn>(original);
    *entry.installed = entry.target;
  }
  if (!ok) {
    if (g_citySerializeSaveTarget != nullptr) {
      (void)g_host->removeHook(g_host->pluginHandle, g_citySerializeSaveTarget);
    }
    if (g_citySerializeLoadTarget != nullptr) {
      (void)g_host->removeHook(g_host->pluginHandle, g_citySerializeLoadTarget);
    }
    g_citySerializeSaveTarget = nullptr;
    g_citySerializeLoadTarget = nullptr;
    g_citySerializeSaveOriginal = nullptr;
    g_citySerializeLoadOriginal = nullptr;
    return false;
  }
  return true;
}

// Installs object lifecycle hooks (idempotent, all-or-nothing). City constructor/destructor and Unit
// destructor are all required for the lifecycle service; failure removes everything.
bool EnsureLifecycleHooks() {
  if (g_host == nullptr || g_host->installHook == nullptr || g_host->removeHook == nullptr ||
      g_engine == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_hookMutex);
  if (g_cityCtorOriginal != nullptr && g_cityDtorOriginal != nullptr &&
      g_unitDtorOriginal != nullptr) {
    return true; // Already installed: reuse; the per-context gate is managed by SetContextActive.
  }
  if (g_engine->cityConstructor == nullptr || g_engine->cityDestructor == nullptr ||
      g_engine->unitDestructor == nullptr) {
    static std::atomic<bool> kLoggedMissing{false};
    if (!kLoggedMissing.exchange(true)) {
      Log(static_cast<int>(bridge::LogLevel::kWarning),
          "persistence: object lifecycle entry unavailable; cleanup/notifications degrade");
    }
    return false;
  }

  void* city_ctor_original = nullptr;
  int status = g_host->installHook(g_host->pluginHandle, g_engine->cityConstructor,
                                   reinterpret_cast<void*>(&CityConstructor_Hook),
                                   &city_ctor_original);
  if (status != 0 || city_ctor_original == nullptr) {
    LogF(static_cast<int>(bridge::LogLevel::kError),
         "persistence: city-constructor hook install -> %d", status);
    (void)g_host->removeHook(g_host->pluginHandle, g_engine->cityConstructor);
    return false;
  }
  g_cityCtorTarget = g_engine->cityConstructor;
  g_cityCtorOriginal = reinterpret_cast<CityConstructorFn>(city_ctor_original);

  void* city_dtor_original = nullptr;
  status = g_host->installHook(g_host->pluginHandle, g_engine->cityDestructor,
                               reinterpret_cast<void*>(&CityDestructor_Hook), &city_dtor_original);
  if (status != 0 || city_dtor_original == nullptr) {
    LogF(static_cast<int>(bridge::LogLevel::kError),
         "persistence: city-destructor hook install -> %d", status);
    (void)g_host->removeHook(g_host->pluginHandle, g_cityCtorTarget);
    g_cityCtorTarget = nullptr;
    g_cityCtorOriginal = nullptr;
    return false;
  }
  g_cityDtorTarget = g_engine->cityDestructor;
  g_cityDtorOriginal = reinterpret_cast<ObjectDestructorFn>(city_dtor_original);

  void* unit_dtor_original = nullptr;
  status = g_host->installHook(g_host->pluginHandle, g_engine->unitDestructor,
                               reinterpret_cast<void*>(&UnitDestructor_Hook), &unit_dtor_original);
  if (status != 0 || unit_dtor_original == nullptr) {
    LogF(static_cast<int>(bridge::LogLevel::kError),
         "persistence: unit-destructor hook install -> %d", status);
    (void)g_host->removeHook(g_host->pluginHandle, g_cityCtorTarget);
    (void)g_host->removeHook(g_host->pluginHandle, g_cityDtorTarget);
    g_cityCtorTarget = nullptr;
    g_cityCtorOriginal = nullptr;
    g_cityDtorTarget = nullptr;
    g_cityDtorOriginal = nullptr;
    return false;
  }
  g_unitDtorTarget = g_engine->unitDestructor;
  g_unitDtorOriginal = reinterpret_cast<ObjectDestructorFn>(unit_dtor_original);

  // Optional (non-fatal): eager unit-created notification on unit construction.
  if (g_engine->unitConstructor != nullptr) {
    void* ctor_original = nullptr;
    const int ctor_status =
        g_host->installHook(g_host->pluginHandle, g_engine->unitConstructor,
                            reinterpret_cast<void*>(&UnitConstructor_Hook), &ctor_original);
    if (ctor_status == 0 && ctor_original != nullptr) {
      g_unitCtorTarget = g_engine->unitConstructor;
      g_unitCtorOriginal = reinterpret_cast<UnitConstructorFn>(ctor_original);
    } else {
      LogF(static_cast<int>(bridge::LogLevel::kWarning),
           "persistence: unit constructor hook install -> %d (non-fatal)", ctor_status);
    }
  }
  return true;
}

void ReleasePendingVariables() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_pendingCities.clear();
}

// Detaches every registered variable without releasing it (engine shutdown may still walk the
// archives; the variables are released at the next context creation).
void DetachAllVariables() {
  std::unordered_map<void*, std::vector<VarHandle>> cities;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    cities.swap(g_cityVars);
    g_pendingCities.reserve(g_pendingCities.size() + cities.size());
    for (auto& entry : cities) {
      g_pendingCities.push_back(std::move(entry.second));
    }
  }
}

void DropMapsWithoutFreeing() {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_cityVars.clear();
  g_pendingCities.clear();
}

bool PersistenceDisabledByEnvironment() {
  char* value = nullptr;
  std::size_t size = 0;
  if (_dupenv_s(&value, &size, "YKKZ000_DISABLE_PERSISTENCE") != 0 || value == nullptr) {
    return false;
  }
  const bool disabled = std::strcmp(value, "1") == 0;
  std::free(value);
  return disabled;
}

// -- API entry points --

int ApiDeclareCityIntVector(const char* name, std::uint32_t capacity) {
  if (name == nullptr || name[0] == '\0' || capacity == 0) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(g_descMutex);
  if (g_descriptorIndex.find(name) != g_descriptorIndex.end()) {
    return 1; // Already declared; idempotent.
  }
  g_descriptorIndex.emplace(name, g_descriptors.size());
  g_descriptors.push_back(Descriptor{name, capacity});
  return 1;
}

void ApiStoreCityIntVector(void* city, const char* name, const std::int32_t* values,
                           std::uint32_t count) {
  if (city == nullptr || values == nullptr) {
    return;
  }
  const std::size_t index = DescriptorIndexForName(name);
  if (index == static_cast<std::size_t>(-1)) {
    return;
  }
  const std::vector<VarHandle> vars = LookupCityVars(city);
  if (index >= vars.size() || vars[index].object == nullptr) {
    return;
  }
  std::uint32_t capacity = 0;
  {
    std::lock_guard<std::mutex> lock(g_descMutex);
    if (index >= g_descriptors.size()) {
      return;
    }
    capacity = g_descriptors[index].capacity;
  }
  void* data = nullptr;
  if (!TryReadAt(vars[index].object, kVarData, data) || data == nullptr) {
    return;
  }
  const std::size_t available = VectorEntryCount(vars[index].object, capacity);
  const std::size_t limit = count < capacity ? count : capacity;
  const std::size_t n = limit < available ? limit : available;
  auto* base = static_cast<std::uint8_t*>(data);
  for (std::size_t i = 0; i < n; ++i) {
    (void)TryWriteAt(base + i * sizeof(std::int32_t), std::size_t{0}, values[i]);
  }
}

int ApiLoadCityIntVector(void* city, const char* name, std::int32_t* out, std::uint32_t capacity) {
  if (out == nullptr || capacity == 0) {
    return 0;
  }
  std::memset(out, 0, static_cast<std::size_t>(capacity) * sizeof(std::int32_t));
  const std::size_t index = DescriptorIndexForName(name);
  if (index == static_cast<std::size_t>(-1)) {
    return 0;
  }
  const std::vector<VarHandle> vars = LookupCityVars(city);
  if (index >= vars.size() || vars[index].object == nullptr) {
    return 0;
  }
  std::uint32_t descriptor_capacity = 0;
  {
    std::lock_guard<std::mutex> lock(g_descMutex);
    if (index >= g_descriptors.size()) {
      return 0;
    }
    descriptor_capacity = g_descriptors[index].capacity;
  }
  void* data = nullptr;
  if (!TryReadAt(vars[index].object, kVarData, data) || data == nullptr) {
    return 0;
  }
  const std::size_t available = VectorEntryCount(vars[index].object, descriptor_capacity);
  const std::size_t limit = capacity < descriptor_capacity ? capacity : descriptor_capacity;
  const std::size_t n = limit < available ? limit : available;
  auto* base = static_cast<const std::uint8_t*>(data);
  bool any = false;
  for (std::size_t i = 0; i < n; ++i) {
    std::int32_t value = 0;
    if (!TryReadAt(base + i * sizeof(std::int32_t), std::size_t{0}, value)) {
      break;
    }
    out[i] = value;
    if (value != 0) {
      any = true;
    }
  }
  return any ? 1 : 0;
}

int ApiRegisterObjectLifecycle(void* user, ObjectLifecycleFn city_created,
                               ObjectLifecycleFn city_destroyed, ObjectLifecycleFn unit_created,
                               ObjectLifecycleFn unit_destroyed) {
  std::lock_guard<std::mutex> lock(g_lifecycleMutex);
  g_subscribers.push_back(Subscriber{user, city_created, city_destroyed, unit_created,
                                     unit_destroyed});
  return 1;
}

// -- Plugin lifecycle (called by the loader) --

// Installs the resident persistence hooks exactly once (from GetPlugin, once the host is bound).
// After this call the hooks live for the whole process; each game context only toggles the data
// gates in SetContextActive, so a repeated context creation can never fail to "reinstall" a hook or
// lose a trampoline. Only DestroyPlugin uninstalls them (UninstallHooks).
bool InstallResidentHooks() {
  if (PersistenceDisabledByEnvironment()) {
    static std::atomic<bool> kLoggedDisabled{false};
    if (!kLoggedDisabled.exchange(true)) {
      Log(static_cast<int>(bridge::LogLevel::kWarning),
          "persistence: disabled by YKKZ000_DISABLE_PERSISTENCE; values will not survive save/load");
    }
    return false;
  }
  if (g_host == nullptr || g_host->installHook == nullptr || g_host->removeHook == nullptr ||
      g_engine == nullptr || g_engine->cityConstructor == nullptr ||
      g_engine->autoVariableRegister == nullptr || g_engine->engineAlignedMalloc == nullptr ||
      g_engine->engineAlignedFree == nullptr) {
    static std::atomic<bool> kLoggedUnavailable{false};
    if (!kLoggedUnavailable.exchange(true)) {
      Log(static_cast<int>(bridge::LogLevel::kWarning),
          "persistence: engine AutoVariable entries unavailable; values will not survive save/load");
    }
    return false;
  }
  // The lifecycle service and the persistence serialization are independent: persistence still works
  // when lifecycle notifications degrade, and vice versa. Each install is idempotent (a repeated
  // call reuses the existing trampoline), and a failure only removes the hooks that this attempt
  // did not complete.
  const bool lifecycle = EnsureLifecycleHooks();
  const bool serialization = EnsureSerializationHooks();
  g_lifecycleHooksReady.store(lifecycle, std::memory_order_release);
  g_serializationHooksReady.store(serialization, std::memory_order_release);
  LogF(static_cast<int>(bridge::LogLevel::kInfo),
       "persistence: hooks installed (lifecycle=%d serialization=%d)", lifecycle ? 1 : 0,
       serialization ? 1 : 0);
  return lifecycle || serialization;
}

void ResetPersistenceForUnload() {
  UninstallHooks();
  DropMapsWithoutFreeing();
}

void PersistenceOnGameContext(bridge::GameContextEvent event, void* /*context*/) {
  SetContextActive(event == bridge::GameContextEvent::kCreated);
}

const PersistenceApi kApiTable = {
    kPersistenceApiVersion,       // version
    &ApiDeclareCityIntVector,     // declare_city_int_vector
    &ApiStoreCityIntVector,       // store_city_int_vector
    &ApiLoadCityIntVector,        // load_city_int_vector
    &ApiRegisterObjectLifecycle,  // register_object_lifecycle
};

} // namespace

} // namespace ykkz000::plugin

extern "C" const ykkz000::plugin::PersistenceApi* GetPersistenceApi(std::uint32_t version) {
  if (version > ykkz000::plugin::kPersistenceApiVersion) {
    return nullptr;
  }
  return &ykkz000::plugin::kApiTable;
}

// -- Plugin entry points --

namespace {

constexpr ykkz000::bridge::PluginManifest kManifest = {
    sizeof(ykkz000::bridge::PluginManifest), // structSize
    ykkz000::bridge::kHostApiVersion,        // apiVersion
    "ykkz000.api.persistence",               // name
    1,                                       // versionMajor
    0,                                       // versionMinor
    0,                                       // versionPatch
    nullptr,                                 // dependencies
    0,                                       // dependencyCount
    ykkz000::bridge::kHostApiVersion,        // requiredHostApi
};

} // namespace

YKKZ000_PLUGIN_API const ykkz000::bridge::PluginManifest* GetPluginManifest() { return &kManifest; }

YKKZ000_PLUGIN_API int GetPlugin(ykkz000::bridge::Host* host) {
  if (host == nullptr || host->apiVersion < ykkz000::bridge::kHostApiVersion ||
      host->engine == nullptr || host->installHook == nullptr) {
    return 0;
  }
  ykkz000::plugin::BindHost(host);
  host->onGameContext = &ykkz000::plugin::PersistenceOnGameContext;
  // Install the resident hooks once, now that the host is bound. The hooks then stay installed for
  // the whole process; game contexts only toggle the data gates (PersistenceOnGameContext).
  (void)ykkz000::plugin::InstallResidentHooks();
  return 1;
}

YKKZ000_PLUGIN_API void DestroyPlugin() { ykkz000::plugin::ResetPersistenceForUnload(); }
