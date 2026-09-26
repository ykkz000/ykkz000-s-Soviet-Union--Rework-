#include <windows.h>

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
};

// —— 候选路径（相对 loader 所在目录）：仅接受同目录被重命名的原版 XP2，
//    或未被替换的同目录 XP2；不再回退到 XP1/Base，避免跨版本误用偏移。
constexpr const wchar_t* kCandidatePaths[] = {
    L"GameCore_XP2_FinalRelease_orig.dll",
    L"GameCore_XP2_FinalRelease_orig.dll.bak",
    L"GameCore_XP2_FinalRelease.dll",
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
  const auto* nt = ntHeaders(module);
  if (nt == nullptr) {
    return false;
  }
  if (nt->FileHeader.TimeDateStamp != kKnownXp2Build.timeDateStamp ||
      nt->OptionalHeader.SizeOfImage != kKnownXp2Build.sizeOfImage) {
    logMessage(1, "GameCore 构建指纹不匹配，拒绝使用固定偏移");
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
  resolved.module = module;
  if (resolved.getEffectRegistry == nullptr || resolved.mallocTemp == nullptr ||
      resolved.reserveVector == nullptr || resolved.effectApply == nullptr ||
      resolved.effectRemove == nullptr || resolved.changeYieldModifier == nullptr) {
    return false;
  }

  out = resolved; // 仅在完整解析成功后提交
  return true;
}

HMODULE tryLoadCandidate(const std::wstring& directory, const wchar_t* relative) {
  std::wstring full = directory + L"\\" + relative;
  if (GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return nullptr;
  }
  HMODULE module = LoadLibraryExW(full.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (module == nullptr) {
    return nullptr;
  }
  if (module == g_selfModule) {
    FreeLibrary(module);
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

std::uint32_t makeHash(const char* text) {
  return bridge::makeHash(text);
}

void logMessage(int level, const char* message) {
  if (message == nullptr) {
    return;
  }
  char buffer[1024] = {};
  const int written = _snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
                                  "[YKKZ000:%d] %s\n", level, message);
  if (written != 0) {
    OutputDebugStringA(buffer);
  }
}

bool ensureGameCoreLoaded() {
  std::lock_guard<std::mutex> guard(g_loadMutex);
  if (g_api.module != nullptr) {
    return true;
  }

  const std::wstring directory = moduleDirectory();
  if (directory.empty()) {
    logMessage(0, "无法确定 loader 所在目录");
    return false;
  }

  for (const wchar_t* relative : kCandidatePaths) {
    HMODULE module = tryLoadCandidate(directory, relative);
    if (module == nullptr) {
      continue;
    }
    GameCoreApi resolved;
    if (resolveApi(module, resolved)) {
      g_api = resolved;
      return true;
    }
    logMessage(1, "已加载候选 GameCore，但特征扫描失败，继续尝试其它路径");
    FreeLibrary(module); // g_api 未被写入，不存在悬空指针
  }

  logMessage(0, "未能定位真实 GameCore 或其内部入口");
  return false;
}

const GameCoreApi& gameCore() {
  return g_api;
}

} // namespace ykkz000::loader
