#include <windows.h>

#include <shlobj.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <ykkz000/bridge/hash.h>

#include "loader_internal.h"

namespace ykkz000::loader {
namespace {

// -- Stable string anchors (build-independent basis for locating code) --
constexpr char kEffectMissingAnchor[] =
    "Could not build a modifier factory because the effect <";
constexpr char kCollectionMissingAnchor[] =
    "Could not build a modifier factory because the collection <";
constexpr char kTypesInsertAnchor[] =
    "INSERT OR IGNORE INTO Types(Type,Kind) VALUES(?,?)";

// -- Version table: lock a specific build by PE build fingerprint (TimeDateStamp + SizeOfImage),
//    then give the absolute RVA of each internal entry. A fingerprint mismatch means refusal; never
//    apply XP2 offsets to another build.
//
//    Note: do not compute addresses via "string anchor + delta" -- that string has several copies
//    in the image, and the copy found by findString() may differ from the one used to derive the
//    delta, causing a global misalignment (once off by +0x12F0). RVAs are given directly and gated
//    by the fingerprint; the anchors are used only for the identity self-check below.
//
//    Locked build: GameCore_XP2_FinalRelease.dll (2024-06-27, SizeOfImage 0xC60000).
//
//    RVA semantic verification record (2026-09-29, each entry decompiled at 0x180000000+rva on the
//    release image):
//      * 0x131B80 ChangePopulation: writes only when `0 < *(int*)(city+0x268)+delta`; reads
//        +0xD8(owner)/+0xA8(city id) to assemble the CityID notice (kind label = 2).
//      * 0x131CF0 ChangeYieldModifier: `v = FUN_18072a920(city+0x4A0)`; disassembly confirms the
//        function returns param_1+0x10, so v = city+0x4B0: v[0](+0x00) is the modifier array
//        pointer, v[2](+0x10) is the count, and v+0x18 is the out-of-range fallback scalar, i.e.
//        data +0x4B0 / count +0x4C0 / fallback +0x4C8 ("+0x4A0 data / +0x4B0 count" was a typo).
//      * 0x12FF20 CalculateYield: base accumulation +0x10; modifier sub-object +0x30 (its
//        accumulation +0x40, initial value 0xFFFF9C00 = -100.0 "no modifier"); modifier sources
//        include religious followers +0x4E0, governor titles +0x510 x GetNumTitles, game effects
//        +0x4B0, projects +0x6F0, the +0x540 array, terrain/resources/power, and more.
//      * 0x83B220 AdjustCityYieldModifier::Apply: iterates +0x18(entry count)/+0x30(amount),
//        +0x08 yields/+0x20 amounts, calling 0x131CF0 per entry, matching the effect.h layout.
//      * 0x8D9110 AdjustPlayerStrengthModifier::Apply: +0x40 Amount; +0x44 != 100 goes through
//        powf exponent stacking; +0x48 cap; +0x4C domain (-1 disables); +0x50 x0x6400 per-mille;
//        +0x54 turn/era scaling; +0x58 scaling by player count; +0x5C total already applied;
//        player identity from ownerObj+0xD8.
//      * 0x944040 ProposedCombat::AdjustPlayerStrengthModifier: unit (+0x128 == playerId) or
//        district (GetOwner() == playerId) writes to +0x2C, otherwise writes to +0x30.
//      * 0x12FC10 TrackedValue::AddStep(this=modifier sub-object(out+0x30), step, u32, u32, stack
//        tooltipKey): merges one modifier detail into this+0x10 (i.e. TrackedValue+0x40). When
//        (this+0x14) is non-zero it also appends a detail entry. step is isomorphic to the
//        sub-object (0x30 bytes: has_min/min/has_max/max/value/flag(+0x14)/detail vector(+0x18));
//        the engine constructs it with has_min/has_max/flag=0 and writes only value (the
//        convenience overload 0x12FCE0 does exactly this); tooltipKey is passed to
//        Localization::Lookup for localized text. Optional for consumers (when missing, the plugin
//        degrades to directly accumulating +0x40).
//      * 0x133780 GetYieldFromPopulation (called at CalculateYield 0x180130013): the per-population
//        yield arrays are at city+0x690/+0x6A0/+0x6A8, unit FixedPoint<8>.
//
//    Anchor RVA self-check: the fingerprint (TimeDateStamp+SizeOfImage) only proves "same game
//    version"; it cannot detect a profile taken from another file of the same name (once globally
//    misaligned by +0x12F0). Therefore three stable string anchors' RVAs in this build are also
//    recorded and compared against the runtime measurement; on mismatch the fixed RVAs are refused.
struct BuildProfile {
  std::uint32_t timeDateStamp;
  std::uint32_t sizeOfImage;
  std::ptrdiff_t rvaGetEffectRegistry;
  std::ptrdiff_t rvaMallocTemp;
  std::ptrdiff_t rvaReserveVector;
  std::ptrdiff_t rvaEffectApply;
  std::ptrdiff_t rvaEffectRemove;
  std::ptrdiff_t rvaEffectStrengthApply;
  std::ptrdiff_t rvaEffectStrengthRemove;
  std::ptrdiff_t rvaGetPlayerByIndex;
  std::ptrdiff_t rvaGetGameManager;
  std::ptrdiff_t rvaStrengthAccumulate;
  std::ptrdiff_t rvaChangeYieldModifier;
  // City::Instance::CalculateYield(YieldTypes, TypeHash, bool) -> TrackedValue (sret):
  // the city yield read path. The release build (0x667C6F5B) entry 0x18012FF20 is verified,
  // 0xECB bytes.
  std::ptrdiff_t rvaCityCalculateYield;
  // TrackedValue::AddStep(this, step, u32, u32, tooltipKey): the modifier-detail append entry. RVA
  // verified; step is isomorphic to the modifier sub-object (civ6::YieldValue), and the tooltip key
  // is passed on the stack (see the verification record above).
  std::ptrdiff_t rvaTrackedValueAddStep;
  // Handler registration: the first three are code; the last two are data (the profiled handler
  // object/descriptor table, not runtime-validated, diagnostics only).
  std::ptrdiff_t rvaHandlerRegistryInit;
  std::ptrdiff_t rvaSetEffectHandler;
  std::ptrdiff_t rvaHandlerNodeInsert;
  std::ptrdiff_t rvaProfiledHandlerData;
  std::ptrdiff_t rvaProfiledHandlerTable;
  // The profiled handler descriptor table's two slots (code): diagnostics only, for the handler
  // apply path.
  std::ptrdiff_t rvaProfiledHandlerAnalyze;
  std::ptrdiff_t rvaProfiledHandlerApply;
  // handler dispatch thunk (code): diagnose bad handler ownership + invalid-handler guard.
  std::ptrdiff_t rvaEffectHandlerDispatch;
  // Anchor RVAs: used for the "identity self-check" (see resolveApi); they corroborate the
  // fingerprint.
  std::ptrdiff_t rvaEffectAnchor;
  std::ptrdiff_t rvaCollectionAnchor;
  std::ptrdiff_t rvaTypesAnchor;
};

constexpr BuildProfile kKnownXp2Build{
    0x667C6F5B,   // TimeDateStamp (matches the actual file)
    0xC60000,     // SizeOfImage (matches the actual file)
    0x95B0C0,     // Registry<IModifierEffectFactory>::GetTypes()
    0x990900,     // Platform::MallocTemp(size)
    0x308F50,     // std::vector<...>::_Reserve / growth
    0x83B220,     // Effects::AdjustCityYieldModifier::Apply
    0x83B5A0,     // Effects::AdjustCityYieldModifier::Remove
    0x8D9110,     // Effects::AdjustPlayerStrengthModifier::Apply
    0x8DA240,     // Effects::AdjustPlayerStrengthModifier::Remove
    0x44F00,      // FUN_180044f00(int playerIndex) -> Player* (no bounds check)
    0x44D60,      // FUN_180044d60() -> GameManager* (+0x50/+0x58 are the player vectors)
    0x944040,     // FUN_180944040(target, playerId, amount): the combat-strength modifier write point
    0x131CF0,     // City::Instance::ChangeYieldModifier(YieldTypes, int)
    0x12FF20,     // City::Instance::CalculateYield(YieldTypes, TypeHash, bool) -> TrackedValue
    0x12FC10,     // TrackedValue::AddStep(this=modifier sub-object, step, u32, u32, tooltipKey)
    0x4891B0,     // FUN_1804891b0(void* root): builds the built-in handler registry
    0x6083F0,     // FUN_1806083f0(root, kind, hash, handlerObj): sets/replaces/removes a handler
    0x489040,     // FUN_180489040(container, outNode, hashPtr): handler table insert/lookup
    0xB65D88,     // profiled handler object (.data, unvalidated)
    0x9FF380,     // its descriptor table [0]=analyzer, [1]=apply (.rdata, unvalidated)
    0x46B0D0,     // descriptor table [0]: FUN_18046b0d0(self, args) analyzer
    0x46B390,     // descriptor table [1]: FUN_18046b390(self, context, args) apply
    0x979290,     // handler dispatch thunk: rcx=[rcx+0x18]; jmp [rax+0x30]
    0xAA4558,     // anchor: "Could not build a modifier factory because the effect <"
    0xAA4500,     // anchor: "… because the collection <"
    0xAA3A80,     // anchor: "INSERT OR IGNORE INTO Types(Type,Kind) VALUES(?,?)"
};

GameCoreApi g_api;
std::mutex g_loadMutex;

const std::uint8_t* imageBegin(HMODULE module) {
  return reinterpret_cast<const std::uint8_t*>(module);
}

const IMAGE_NT_HEADERS* ntHeaders(HMODULE module) {
  const auto* base = imageBegin(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    return nullptr;
  }
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) {
    return nullptr;
  }
  return nt;
}

std::size_t imageSize(HMODULE module) {
  const auto* nt = ntHeaders(module);
  return nt != nullptr ? nt->OptionalHeader.SizeOfImage : 0;
}

bool isExecutableAddress(HMODULE module, const void* address) {
  const auto* nt = ntHeaders(module);
  if (nt == nullptr) {
    return false;
  }
  const auto* base = imageBegin(module);
  const auto* p = static_cast<const std::uint8_t*>(address);
  const auto* section = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
    if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
      continue;
    }
    const auto* start = base + section->VirtualAddress;
    const auto* end = start + section->Misc.VirtualSize;
    if (p >= start && p < end) {
      return true;
    }
  }
  return false;
}

const std::uint8_t* findString(HMODULE module, const char* text) {
  const auto* begin = imageBegin(module);
  const std::size_t size = imageSize(module);
  const std::size_t length = std::strlen(text);
  if (size < length || length == 0) {
    return nullptr;
  }
  const auto* end = begin + size - length;
  for (const std::uint8_t* cursor = begin; cursor <= end; ++cursor) {
    if (cursor[0] == static_cast<std::uint8_t>(text[0]) &&
        std::memcmp(cursor, text, length) == 0) {
      return cursor;
    }
  }
  return nullptr;
}

// Record the actual RVA of a resolved entry. Note: expected here is exactly the source of
// base+expected, so this comparison is always equal (pure arithmetic check) and cannot be used to
// detect "a different build was loaded". The real identity self-check is the anchor-RVA runtime
// comparison in resolveApi.
void logRva(const char* name, void* function, HMODULE module, std::ptrdiff_t expected) {
  if (function == nullptr) {
    logMessageF(0, "%s: <null>", name);
    return;
  }
  const auto actual = static_cast<std::ptrdiff_t>(
      reinterpret_cast<const std::uint8_t*>(function) -
      reinterpret_cast<const std::uint8_t*>(module));
  logMessageF(1, "%s: rva=0x%zX (profile 0x%zX, arithmetic check)",
              name, static_cast<std::size_t>(actual), static_cast<std::size_t>(expected));
}

bool resolveApi(HMODULE module, GameCoreApi& out) {
  LogScope scope("resolve GameCore entry points");
  const auto* nt = ntHeaders(module);
  if (nt == nullptr) {
    return false;
  }
  if (nt->FileHeader.TimeDateStamp != kKnownXp2Build.timeDateStamp ||
      nt->OptionalHeader.SizeOfImage != kKnownXp2Build.sizeOfImage) {
    char detail[256] = {};
    _snprintf_s(detail, sizeof(detail), _TRUNCATE,
                "GameCore build fingerprint mismatch: TimeDateStamp=0x%08X "
                "(expected 0x%08X) SizeOfImage=0x%X (expected 0x%X)",
                nt->FileHeader.TimeDateStamp, kKnownXp2Build.timeDateStamp,
                nt->OptionalHeader.SizeOfImage, kKnownXp2Build.sizeOfImage);
    logMessage(1, detail);
    return false;
  }

  const auto* effectAnchor = findString(module, kEffectMissingAnchor);
  const auto* collectionAnchor = findString(module, kCollectionMissingAnchor);
  const auto* typesAnchor = findString(module, kTypesInsertAnchor);
  if (effectAnchor == nullptr || collectionAnchor == nullptr || typesAnchor == nullptr) {
    logMessage(0, "GameCore anchor strings missing; build may be unexpected");
    return false;
  }

  const auto* base = imageBegin(module);
  const std::size_t size = imageSize(module);

  // Measured anchor RVAs (scanned from the actually loaded image at runtime, then converted using
  // the image base).
  const std::ptrdiff_t effectAnchorRva =
      static_cast<std::ptrdiff_t>(effectAnchor - base);
  const std::ptrdiff_t collectionAnchorRva =
      static_cast<std::ptrdiff_t>(collectionAnchor - base);
  const std::ptrdiff_t typesAnchorRva =
      static_cast<std::ptrdiff_t>(typesAnchor - base);
  logMessageF(1, "anchor rva: effect=0x%zX collection=0x%zX types=0x%zX",
              static_cast<std::size_t>(effectAnchorRva),
              static_cast<std::size_t>(collectionAnchorRva),
              static_cast<std::size_t>(typesAnchorRva));

  // The measured anchor RVAs must match the profile: this is the check that can really detect
  // "a different build was loaded".
  if (effectAnchorRva != kKnownXp2Build.rvaEffectAnchor ||
      collectionAnchorRva != kKnownXp2Build.rvaCollectionAnchor ||
      typesAnchorRva != kKnownXp2Build.rvaTypesAnchor) {
    logMessageF(0,
                "Anchor RVA mismatch: the loaded GameCore differs from the profiled build "
                "(effect 0x%zX/0x%zX, collection 0x%zX/0x%zX, types 0x%zX/0x%zX); "
                "refusing to use fixed RVAs",
                static_cast<std::size_t>(effectAnchorRva),
                static_cast<std::size_t>(kKnownXp2Build.rvaEffectAnchor),
                static_cast<std::size_t>(collectionAnchorRva),
                static_cast<std::size_t>(kKnownXp2Build.rvaCollectionAnchor),
                static_cast<std::size_t>(typesAnchorRva),
                static_cast<std::size_t>(kKnownXp2Build.rvaTypesAnchor));
    return false;
  }

  // New version-table entries give the RVA directly (no longer relative to an anchor); they are
  // likewise required to fall in an executable section.
  auto computeRva = [&](std::ptrdiff_t rva) -> void* {
    if (rva <= 0) {
      return nullptr;
    }
    const auto* candidate = base + rva;
    if (candidate < base || candidate >= base + size) {
      return nullptr;
    }
    if (!isExecutableAddress(module, candidate)) {
      return nullptr;
    }
    return const_cast<std::uint8_t*>(candidate);
  };

  // Data entries (handler object, descriptor table) live in .data/.rdata; only the image range is
  // checked, not executability.
  auto computeDataRva = [&](std::ptrdiff_t rva) -> void* {
    if (rva <= 0) {
      return nullptr;
    }
    const auto* candidate = base + rva;
    if (candidate < base || candidate >= base + size) {
      return nullptr;
    }
    return const_cast<std::uint8_t*>(candidate);
  };

  GameCoreApi resolved;
  resolved.getEffectRegistry = computeRva(kKnownXp2Build.rvaGetEffectRegistry);
  resolved.mallocTemp = computeRva(kKnownXp2Build.rvaMallocTemp);
  resolved.reserveVector = computeRva(kKnownXp2Build.rvaReserveVector);
  resolved.effectApply = computeRva(kKnownXp2Build.rvaEffectApply);
  resolved.effectRemove = computeRva(kKnownXp2Build.rvaEffectRemove);
  resolved.effectStrengthApply = computeRva(kKnownXp2Build.rvaEffectStrengthApply);
  resolved.effectStrengthRemove = computeRva(kKnownXp2Build.rvaEffectStrengthRemove);
  resolved.getPlayerByIndex = computeRva(kKnownXp2Build.rvaGetPlayerByIndex);
  resolved.getGameManager = computeRva(kKnownXp2Build.rvaGetGameManager);
  resolved.strengthAccumulate = computeRva(kKnownXp2Build.rvaStrengthAccumulate);
  resolved.changeYieldModifier = computeRva(kKnownXp2Build.rvaChangeYieldModifier);
  resolved.cityCalculateYield = computeRva(kKnownXp2Build.rvaCityCalculateYield);
  resolved.trackedValueAddStep = computeRva(kKnownXp2Build.rvaTrackedValueAddStep);
  resolved.handlerRegistryInit = computeRva(kKnownXp2Build.rvaHandlerRegistryInit);
  resolved.setEffectHandler = computeRva(kKnownXp2Build.rvaSetEffectHandler);
  resolved.handlerNodeInsert = computeRva(kKnownXp2Build.rvaHandlerNodeInsert);
  resolved.profiledHandlerData = computeDataRva(kKnownXp2Build.rvaProfiledHandlerData);
  resolved.profiledHandlerTable = computeDataRva(kKnownXp2Build.rvaProfiledHandlerTable);
  resolved.profiledHandlerAnalyze = computeRva(kKnownXp2Build.rvaProfiledHandlerAnalyze);
  resolved.profiledHandlerApply = computeRva(kKnownXp2Build.rvaProfiledHandlerApply);
  resolved.effectHandlerDispatch = computeRva(kKnownXp2Build.rvaEffectHandlerDispatch);
  resolved.module = module;
  // Record each entry's actual RVA (pure arithmetic check; identity verification was already done
  // by the anchor comparison above).
  logRva("getEffectRegistry", resolved.getEffectRegistry, module,
         kKnownXp2Build.rvaGetEffectRegistry);
  logRva("mallocTemp", resolved.mallocTemp, module, kKnownXp2Build.rvaMallocTemp);
  logRva("reserveVector", resolved.reserveVector, module,
         kKnownXp2Build.rvaReserveVector);
  logRva("effectApply", resolved.effectApply, module, kKnownXp2Build.rvaEffectApply);
  logRva("effectRemove", resolved.effectRemove, module, kKnownXp2Build.rvaEffectRemove);
  logRva("effectStrengthApply", resolved.effectStrengthApply, module,
         kKnownXp2Build.rvaEffectStrengthApply);
  logRva("effectStrengthRemove", resolved.effectStrengthRemove, module,
         kKnownXp2Build.rvaEffectStrengthRemove);
  logRva("getPlayerByIndex", resolved.getPlayerByIndex, module,
         kKnownXp2Build.rvaGetPlayerByIndex);
  logRva("getGameManager", resolved.getGameManager, module,
         kKnownXp2Build.rvaGetGameManager);
  logRva("strengthAccumulate", resolved.strengthAccumulate, module,
         kKnownXp2Build.rvaStrengthAccumulate);
  logRva("changeYieldModifier", resolved.changeYieldModifier, module,
         kKnownXp2Build.rvaChangeYieldModifier);
  logRva("cityCalculateYield", resolved.cityCalculateYield, module,
         kKnownXp2Build.rvaCityCalculateYield);
  logRva("trackedValueAddStep", resolved.trackedValueAddStep, module,
         kKnownXp2Build.rvaTrackedValueAddStep);
  logRva("handlerRegistryInit", resolved.handlerRegistryInit, module,
         kKnownXp2Build.rvaHandlerRegistryInit);
  logRva("setEffectHandler", resolved.setEffectHandler, module,
         kKnownXp2Build.rvaSetEffectHandler);
  logRva("handlerNodeInsert", resolved.handlerNodeInsert, module,
         kKnownXp2Build.rvaHandlerNodeInsert);
  logMessageF(1, "handler data: profiledHandlerData=%p profiledHandlerTable=%p",
              resolved.profiledHandlerData, resolved.profiledHandlerTable);
  logRva("profiledHandlerAnalyze", resolved.profiledHandlerAnalyze, module,
         kKnownXp2Build.rvaProfiledHandlerAnalyze);
  logRva("profiledHandlerApply", resolved.profiledHandlerApply, module,
         kKnownXp2Build.rvaProfiledHandlerApply);
  logRva("effectHandlerDispatch", resolved.effectHandlerDispatch, module,
         kKnownXp2Build.rvaEffectHandlerDispatch);
  // cityCalculateYield / trackedValueAddStep are not fatal checks: when missing, the plugin skips
  // the corresponding hook/injection and the effect degrades gracefully, rather than preventing the
  // whole Loader from initializing.
  if (resolved.getEffectRegistry == nullptr || resolved.mallocTemp == nullptr ||
      resolved.reserveVector == nullptr || resolved.effectApply == nullptr ||
      resolved.effectRemove == nullptr || resolved.changeYieldModifier == nullptr) {
    return false;
  }

  out = resolved; // committed only after a complete successful resolution
  return true;
}

std::wstring parentDirectory(const std::wstring& path) {
  const std::size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
}

std::wstring hostExecutableDirectory() {
  wchar_t buffer[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(GetModuleHandleW(nullptr), buffer, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return {};
  }
  return parentDirectory(std::wstring(buffer, length));
}

std::vector<std::wstring> candidatePaths(const std::wstring& loaderDirectory) {
  std::vector<std::wstring> paths;
  // 1) In-place replacement deployment: the renamed original next to the loader
  if (!loaderDirectory.empty()) {
    paths.push_back(loaderDirectory + L"\\GameCore_XP2_FinalRelease_orig.dll");
    paths.push_back(loaderDirectory + L"\\GameCore_XP2_FinalRelease_orig.dll.bak");
    paths.push_back(loaderDirectory + L"\\GameCore_XP2_FinalRelease.dll");
  }
  // 2) Mod-directory deployment: walk up from the host EXE (<root>\Base\Binaries\Win64*) to the
  // game root, locating DLC/Expansion2
  std::wstring directory = hostExecutableDirectory();
  for (int up = 0; up < 5 && !directory.empty(); ++up) {
    paths.push_back(directory +
                    L"\\DLC\\Expansion2\\Binaries\\Win64\\GameCore_XP2_FinalRelease.dll");
    directory = parentDirectory(directory);
  }
  return paths;
}

HMODULE tryLoadPath(const std::wstring& full) {
  if (full.empty() || GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return nullptr;
  }
  HMODULE module = LoadLibraryExW(full.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (module == nullptr || module == g_selfModule) {
    if (module != nullptr) {
      FreeLibrary(module);
    }
    return nullptr;
  }
  return module;
}

// Build the plugin-visible EngineApi (a read-only set of function pointers) from the resolved
// GameCoreApi. Only "mechanism entries" are exposed, with no behavior policy; proposedCombatAdjust
// is the combat-strength modifier write point FUN_180944040 (DB name
// GameEffects::ProposedCombat::AdjustPlayerStrengthModifier).
bridge::EngineApi g_engineApi;
void publishEngineApi(const GameCoreApi& api) {
  bridge::EngineApi engine = {};
  engine.effectApply = api.effectApply;
  engine.effectRemove = api.effectRemove;
  engine.effectStrengthApply = api.effectStrengthApply;
  engine.effectStrengthRemove = api.effectStrengthRemove;
  engine.proposedCombatAdjust = api.strengthAccumulate;
  engine.changeYieldModifier = api.changeYieldModifier;
  engine.cityCalculateYield = api.cityCalculateYield; // added in v7
  engine.trackedValueAddStep = api.trackedValueAddStep; // added in v8
  engine.getPlayer = api.getPlayerByIndex;
  engine.getGameManager = api.getGameManager;
  g_engineApi = engine;
}

} // namespace

std::wstring moduleDirectory() {
  wchar_t buffer[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(g_selfModule, buffer, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return {};
  }
  std::wstring path(buffer, length);
  const std::size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
}

namespace {

std::wstring gameLogDirectory() {
  // Prefer the Known Folder API; fall back to the environment variable on failure.
  std::wstring local;
  PWSTR wide = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &wide))) {
    local.assign(wide);
    CoTaskMemFree(wide);
  } else {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
      local.assign(buffer, length);
    }
  }
  if (local.empty()) {
    return {};
  }
  const std::wstring dir =
      local + L"\\Firaxis Games\\Sid Meier's Civilization VI\\Logs";
  CreateDirectoryW(dir.c_str(), nullptr); // usually already exists; failure is not fatal
  return dir;
}

std::wstring logFilePath() {
  const std::wstring dir = gameLogDirectory();
  if (!dir.empty()) {
    return dir + L"\\YKKZ000_loader.log";
  }
  const std::wstring fallback = moduleDirectory(); // fallback: the Loader directory
  return fallback.empty() ? std::wstring{} : fallback + L"\\YKKZ000_loader.log";
}

} // namespace

std::uint32_t makeHash(const char* text) {
  return bridge::makeHash(text);
}

void logMessage(int level, const char* message) {
  if (message == nullptr) {
    return;
  }
  char buffer[1024] = {};
  _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "[YKKZ000:%d] %s\n", level, message);
  const std::size_t length = std::strlen(buffer);
  if (length == 0) {
    return;
  }
  OutputDebugStringA(buffer);

  static std::mutex s_logMutex;
  std::lock_guard<std::mutex> guard(s_logMutex);
  static const std::wstring s_logPath = logFilePath(); // resolved only once per process
  if (s_logPath.empty()) {
    return;
  }
  HANDLE file = CreateFileW(s_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }
  DWORD written = 0;
  WriteFile(file, buffer, static_cast<DWORD>(length), &written, nullptr);
  CloseHandle(file);
}

void logMessageF(int level, const char* format, ...) {
  if (format == nullptr) {
    return;
  }
  char buffer[1024] = {};
  va_list args;
  va_start(args, format);
  _vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, format, args);
  va_end(args);
  logMessage(level, buffer);
}

void logMessage(int level, const std::wstring& message) {
  if (message.empty()) {
    return;
  }
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, message.c_str(),
                                        static_cast<int>(message.size()),
                                        nullptr, 0, nullptr, nullptr);
  if (bytes <= 0) {
    return;
  }
  std::string utf8(static_cast<std::size_t>(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, 0, message.c_str(), static_cast<int>(message.size()),
                      utf8.data(), bytes, nullptr, nullptr);
  logMessage(level, utf8.c_str());
}

bool ensureGameCoreLoaded() {
  std::lock_guard<std::mutex> guard(g_loadMutex);
  LogScope scope("locate real GameCore");
  if (g_api.module != nullptr) {
    return true;
  }
  const std::wstring directory = moduleDirectory();
  if (directory.empty()) {
    logMessage(0, "Unable to determine the loader directory");
    return false;
  }
  logMessage(1, L"Loader directory: " + directory);

  const std::vector<std::wstring> candidates = candidatePaths(directory);
  logMessage(1, L"Real GameCore candidate path count: " + std::to_wstring(candidates.size()));
  for (const std::wstring& path : candidates) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
      logMessage(2, L"Candidate does not exist: " + path);
      continue;
    }
    logMessage(1, L"Trying candidate GameCore: " + path);
    HMODULE module = tryLoadPath(path);
    if (module == nullptr) {
      logMessage(1, L"Candidate load failed: " + path);
      continue;
    }
    GameCoreApi resolved;
    if (resolveApi(module, resolved)) {
      g_api = resolved;
      publishEngineApi(resolved);
      logMessageF(1, "GameCore module=%p base=%p", module, imageBegin(module));
      logMessage(1, L"Loaded real GameCore: " + path);
      return true;
    }
    logMessage(1, L"Candidate fingerprint/signature mismatch: " + path);
    FreeLibrary(module);
  }

  // Fallback: let the game DLL search path resolve the standard name
  HMODULE module = LoadLibraryW(L"GameCore_XP2_FinalRelease.dll");
  if (module != nullptr && module != g_selfModule) {
    wchar_t buffer[MAX_PATH] = {};
    GetModuleFileNameW(module, buffer, MAX_PATH);
    GameCoreApi resolved;
    if (resolveApi(module, resolved)) {
      g_api = resolved;
      publishEngineApi(resolved);
      logMessageF(1, "GameCore module=%p base=%p", module, imageBegin(module));
      logMessage(1, std::wstring(L"Loaded real GameCore via DLL search path: ") + buffer);
      return true;
    }
    FreeLibrary(module);
  } else if (module == g_selfModule && module != nullptr) {
    FreeLibrary(module);
  }
  logMessage(0, "Failed to locate the real GameCore");
  return false;
}

const GameCoreApi& gameCore() {
  return g_api;
}

const bridge::EngineApi& engineApi() {
  return g_engineApi;
}

} // namespace ykkz000::loader
