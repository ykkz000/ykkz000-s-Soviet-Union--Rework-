#include <windows.h>

#include <shlobj.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <ykkz000/hash.h>

#include "loader_internal.h"

namespace ykkz000::loader {
namespace {

// —— 稳定字符串锚点（与具体构建无关的定位依据）——
constexpr char kEffectMissingAnchor[] =
    "Could not build a modifier factory because the effect <";
constexpr char kCollectionMissingAnchor[] =
    "Could not build a modifier factory because the collection <";
constexpr char kTypesInsertAnchor[] =
    "INSERT OR IGNORE INTO Types(Type,Kind) VALUES(?,?)";

// —— 版本表：以 PE 构建指纹（TimeDateStamp + SizeOfImage）锁定具体构建，
//    再给出各内部入口相对 kEffectMissingAnchor 的字节偏移。指纹或锚点任一
//    不匹配即拒绝，绝不把 XP2 偏移用于其它构建。新增构建时追加条目。
//
//    已锁定的构建：GameCore_XP2_FinalRelease.dll（2024-06-27，SizeOfImage 0xC60000）。
//      锚点 0x180AA3268；GetTypes 0x18095A950；MallocTemp 0x1809901A0；_Reserve 0x180985B90。
struct BuildProfile {
  std::uint32_t timeDateStamp;
  std::uint32_t sizeOfImage;
  std::ptrdiff_t deltaGetEffectRegistry;
  std::ptrdiff_t deltaMallocTemp;
  std::ptrdiff_t deltaReserveVector;
  std::ptrdiff_t rvaEffectApply;
  std::ptrdiff_t rvaEffectRemove;
  std::ptrdiff_t rvaChangeYieldModifier;
  std::ptrdiff_t rvaChangePopulation;
};

constexpr BuildProfile kKnownXp2Build{
    0x667C6F5B,
    0xC60000,
    -static_cast<std::ptrdiff_t>(0x148918),
    -static_cast<std::ptrdiff_t>(0x1130C8),
    -static_cast<std::ptrdiff_t>(0x11D6D8),
    0x833930,   // Effects::AdjustCityYieldModifier::Apply
    0x8343A0,   // Effects::AdjustCityYieldModifier::Remove
    0x131E60,   // City::Instance::ChangeYieldModifier(YieldTypes, int)
    0x131CF0,   // City::Instance::ChangePopulation(int delta)
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
    return false;
  }

  const auto* base = imageBegin(module);
  const std::size_t size = imageSize(module);
  const auto* anchor = effectAnchor;

  auto compute = [&](std::ptrdiff_t delta) -> void* {
    const auto* candidate = anchor + delta;
    if (candidate < base || candidate >= base + size) {
      return nullptr;
    }
    if (!isExecutableAddress(module, candidate)) {
      return nullptr;
    }
    return const_cast<std::uint8_t*>(candidate);
  };

  // 版本表新增项直接给出 RVA（不再相对锚点），同样要求落在可执行段。
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

  GameCoreApi resolved;
  resolved.getEffectRegistry = compute(kKnownXp2Build.deltaGetEffectRegistry);
  resolved.mallocTemp = compute(kKnownXp2Build.deltaMallocTemp);
  resolved.reserveVector = compute(kKnownXp2Build.deltaReserveVector);
  resolved.effectApply = computeRva(kKnownXp2Build.rvaEffectApply);
  resolved.effectRemove = computeRva(kKnownXp2Build.rvaEffectRemove);
  resolved.changeYieldModifier = computeRva(kKnownXp2Build.rvaChangeYieldModifier);
  resolved.changePopulation = computeRva(kKnownXp2Build.rvaChangePopulation);
  resolved.module = module;
  // changePopulation 不列入致命检查：缺失时 installPopulationHook() 会退化为
  // 建立时人口快照，而不是让整个 Loader 无法初始化。
  if (resolved.getEffectRegistry == nullptr || resolved.mallocTemp == nullptr ||
      resolved.reserveVector == nullptr || resolved.effectApply == nullptr ||
      resolved.effectRemove == nullptr || resolved.changeYieldModifier == nullptr) {
    return false;
  }

  out = resolved; // 仅在完整解析成功后提交
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
  // 1) 就地替换部署：loader 同目录下被重命名的原版
  if (!loaderDirectory.empty()) {
    paths.push_back(loaderDirectory + L"\\GameCore_XP2_FinalRelease_orig.dll");
    paths.push_back(loaderDirectory + L"\\GameCore_XP2_FinalRelease_orig.dll.bak");
    paths.push_back(loaderDirectory + L"\\GameCore_XP2_FinalRelease.dll");
  }
  // 2) 模组目录部署：宿主 EXE（<root>\Base\Binaries\Win64*）上溯游戏根，定位 DLC/Expansion2
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
  // 优先 Known Folder API；失败退回环境变量。
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
  CreateDirectoryW(dir.c_str(), nullptr); // 目录通常已存在；失败也不致命
  return dir;
}

std::wstring logFilePath() {
  const std::wstring dir = gameLogDirectory();
  if (!dir.empty()) {
    return dir + L"\\YKKZ000_loader.log";
  }
  const std::wstring fallback = moduleDirectory(); // 兜底：Loader 目录
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
  static const std::wstring s_logPath = logFilePath(); // 进程内只解析一次
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
      logMessage(1, L"Loaded real GameCore: " + path);
      return true;
    }
    logMessage(1, L"Candidate fingerprint/signature mismatch: " + path);
    FreeLibrary(module);
  }

  // 兜底：交由游戏 DLL 搜索路径解析标准名
  HMODULE module = LoadLibraryW(L"GameCore_XP2_FinalRelease.dll");
  if (module != nullptr && module != g_selfModule) {
    wchar_t buffer[MAX_PATH] = {};
    GetModuleFileNameW(module, buffer, MAX_PATH);
    GameCoreApi resolved;
    if (resolveApi(module, resolved)) {
      g_api = resolved;
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

} // namespace ykkz000::loader
