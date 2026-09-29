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

// —— 版本表：以 PE 构建指纹（TimeDateStamp + SizeOfImage）锁定具体构建，再给出各内部
//    入口的绝对 RVA。指纹不匹配即拒绝，绝不把 XP2 偏移用于其它构建。
//
//    注意：不用"字符串锚点 + delta"来算地址——该字符串在镜像中存在多处副本，
//    findString() 命中的副本可能与推导 delta 时的副本不同，导致整体错位（曾偏移
//    +0x12F0）。RVA 直接给出并由 fingerprint 门控；锚点仅用于下面的身份自检。
//
//    已锁定的构建：GameCore_XP2_FinalRelease.dll（2024-06-27，SizeOfImage 0xC60000）。
//
//    RVA 语义核验记录（2026-09-29，在发布镜像上逐条反编译 0x180000000+rva 确认）：
//      * 0x131B80 ChangePopulation：`0 < *(int*)(city+0x268)+delta` 才写；读
//        +0xD8(owner)/+0xA8(city id) 组装 CityID（kind 标签 = 2）通知。
//      * 0x131CF0 ChangeYieldModifier：`v = FUN_18072a920(city+0x4A0)`，反汇编确认该
//        函数返回 param_1+0x10，故 v = city+0x4B0：v[0](+0x00) 为修正数组指针、v[2](+0x10)
//        为计数、v+0x18 为越界回落标量，即数据 +0x4B0 / 计数 +0x4C0 / 回落 +0x4C8
//        （此前"+0x4A0 数据 / +0x4B0 计数"系笔误）。
//      * 0x12FF20 CalculateYield：基础累计 +0x10；修正子对象 +0x30（其累计 +0x40，初值
//        0xFFFF9C00 = -100.0"无修正"）；修正来源含宗教追随者 +0x4E0、总督头衔 +0x510 ×
//        GetNumTitles、game effects +0x4B0、项目 +0x6F0、+0x540 数组、地形/资源/电力等。
//      * 0x83B220 AdjustCityYieldModifier::Apply：遍历 +0x18(条目数)/+0x30(数量)、
//        +0x08 yields/+0x20 amounts，逐条调 0x131CF0，与 effect.h 布局一致。
//      * 0x8D9110 AdjustPlayerStrengthModifier::Apply：+0x40 Amount；+0x44!=100 走 powf
//        指数叠加；+0x48 上限；+0x4C 域(-1 关闭)；+0x50 ×0x6400 千分比；+0x54 回合/时代
//        缩放；+0x58 按玩家数缩放；+0x5C 已应用总量；玩家身份取 ownerObj+0xD8。
//      * 0x944040 ProposedCombat::AdjustPlayerStrengthModifier：单位(+0x128==playerId)
//        或区域(GetOwner()==playerId) 写入 +0x2C，否则写入 +0x30。
//      * 0x12FC10 TrackedValue::AddStep(this=修正子对象, step, u32, u32)：把一条修正明细
//        并入 this+0x10（即 TrackedValue+0x40）；step 结构与 tooltip 键的约定待确认，
//        当前仅暴露入口备查，不用于逻辑（读取路径仍直接累加 +0x40）。
//      * 0x133780 GetYieldFromPopulation（CalculateYield 0x180130013 处调用）：每人口产出
//        数组位于 city+0x690/+0x6A0/+0x6A8，单位 FixedPoint<8>。
//
//    锚点 RVA 自检：指纹（TimeDateStamp+SizeOfImage）只能证明“是同一版游戏”，
//    无法发现 profile 取自另一份同名文件（曾整体错位 +0x12F0）。因此额外记录三个
//    稳定字符串锚点在本构建中的 RVA，运行期实测比对；不一致即拒绝使用固定 RVA。
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
  std::ptrdiff_t rvaChangePopulation;
  // City::Instance::CalculateYield(YieldTypes, TypeHash, bool) -> TrackedValue（sret）：
  // 城市产出读取路径。发布构建（0x667C6F5B）入口 0x18012FF20 已核验，0xECB 字节。
  std::ptrdiff_t rvaCityCalculateYield;
  // TrackedValue::AddStep(this, step, u32, u32)：修正明细追加入口。RVA 已核验；step 结构
  // 与 tooltip 键的参数约定待确认，仅暴露备查（见上方核验记录）。
  std::ptrdiff_t rvaTrackedValueAddStep;
  // 处理器注册（handler registry）：前三项为代码；后两项为数据（handler 对象/描述表，
  // 未经运行期校验，仅作诊断）。
  std::ptrdiff_t rvaHandlerRegistryInit;
  std::ptrdiff_t rvaSetEffectHandler;
  std::ptrdiff_t rvaHandlerNodeInsert;
  std::ptrdiff_t rvaTemplateEffectHandler;
  std::ptrdiff_t rvaTemplateHandlerTable;
  // 模板描述表的两个槽（代码）：仅用于诊断 handler 应用路径。
  std::ptrdiff_t rvaTemplateAnalyze;
  std::ptrdiff_t rvaTemplateApply;
  // handler 派发 thunk（代码）：诊断坏 handler 归属 + 无效 handler 守卫。
  std::ptrdiff_t rvaEffectHandlerDispatch;
  // 锚点 RVA：用于"身份自检"（见 resolveApi），可与指纹互为印证。
  std::ptrdiff_t rvaEffectAnchor;
  std::ptrdiff_t rvaCollectionAnchor;
  std::ptrdiff_t rvaTypesAnchor;
};

constexpr BuildProfile kKnownXp2Build{
    0x667C6F5B,   // TimeDateStamp（与实际文件一致）
    0xC60000,     // SizeOfImage（与实际文件一致）
    0x95B0C0,     // Registry<IModifierEffectFactory>::GetTypes()
    0x990900,     // Platform::MallocTemp(size)
    0x308F50,     // std::vector<...>::_Reserve / 扩容
    0x83B220,     // Effects::AdjustCityYieldModifier::Apply
    0x83B5A0,     // Effects::AdjustCityYieldModifier::Remove
    0x8D9110,     // Effects::AdjustPlayerStrengthModifier::Apply
    0x8DA240,     // Effects::AdjustPlayerStrengthModifier::Remove
    0x44F00,      // FUN_180044f00(int playerIndex) -> Player*（无边界检查）
    0x44D60,      // FUN_180044d60() -> GameManager*（+0x50/+0x58 为玩家向量）
    0x944040,     // FUN_180944040(target, playerId, amount)：战斗力修正的写入点
    0x131CF0,     // City::Instance::ChangeYieldModifier(YieldTypes, int)
    0x131B80,     // City::Instance::ChangePopulation(int delta)
    0x12FF20,     // City::Instance::CalculateYield(YieldTypes, TypeHash, bool) -> TrackedValue
    0x12FC10,     // TrackedValue::AddStep(this, step, u32, u32)：修正明细追加（参数待确认）
    0x4891B0,     // FUN_1804891b0(void* root)：建立内建 handler 注册表
    0x6083F0,     // FUN_1806083f0(root, kind, hash, handlerObj)：设置/替换/移除 handler
    0x489040,     // FUN_180489040(container, outNode, hashPtr)：handler 表插入/查找
    0xB65D88,     // EFFECT_ADJUST_CITY_YIELD_MODIFIER 的 handler 对象（.data，未经校验）
    0x9FF380,     // 其描述表 [0]=analyzer、[1]=apply（.rdata，未经校验）
    0x46B0D0,     // 描述表 [0]：FUN_18046b0d0(self, args) analyzer
    0x46B390,     // 描述表 [1]：FUN_18046b390(self, context, args) apply
    0x979290,     // handler 派发 thunk：rcx=[rcx+0x18]; jmp [rax+0x30]
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

// 记录解析出的入口实际 RVA。注意：这里的 expected 就是 base+expected 的来源，
// 因此该比较恒为相等（纯算术校验），不能用来发现"加载了别的构建"。
// 真正的身份自检由 resolveApi 中的锚点 RVA 实测比对完成。
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

  // 实测锚点 RVA（运行期从实际加载的镜像扫描得到，再用镜像基址换算）。
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

  // 实测锚点 RVA 必须与 profile 一致：这是真正能发现"加载了别的构建"的校验。
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

  // 数据入口（handler 对象、描述表）位于 .data/.rdata，只校验镜像范围，不要求可执行。
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
  resolved.changePopulation = computeRva(kKnownXp2Build.rvaChangePopulation);
  resolved.cityCalculateYield = computeRva(kKnownXp2Build.rvaCityCalculateYield);
  resolved.trackedValueAddStep = computeRva(kKnownXp2Build.rvaTrackedValueAddStep);
  resolved.handlerRegistryInit = computeRva(kKnownXp2Build.rvaHandlerRegistryInit);
  resolved.setEffectHandler = computeRva(kKnownXp2Build.rvaSetEffectHandler);
  resolved.handlerNodeInsert = computeRva(kKnownXp2Build.rvaHandlerNodeInsert);
  resolved.templateEffectHandler = computeDataRva(kKnownXp2Build.rvaTemplateEffectHandler);
  resolved.templateHandlerTable = computeDataRva(kKnownXp2Build.rvaTemplateHandlerTable);
  resolved.templateAnalyze = computeRva(kKnownXp2Build.rvaTemplateAnalyze);
  resolved.templateApply = computeRva(kKnownXp2Build.rvaTemplateApply);
  resolved.effectHandlerDispatch = computeRva(kKnownXp2Build.rvaEffectHandlerDispatch);
  resolved.module = module;
  // 记录各入口实际 RVA（纯算术校验；身份校验已在上面的锚点比对中完成）。
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
  logRva("changePopulation", resolved.changePopulation, module,
         kKnownXp2Build.rvaChangePopulation);
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
  logMessageF(1, "handler data: templateEffectHandler=%p templateHandlerTable=%p",
              resolved.templateEffectHandler, resolved.templateHandlerTable);
  logRva("templateAnalyze", resolved.templateAnalyze, module,
         kKnownXp2Build.rvaTemplateAnalyze);
  logRva("templateApply", resolved.templateApply, module, kKnownXp2Build.rvaTemplateApply);
  logRva("effectHandlerDispatch", resolved.effectHandlerDispatch, module,
         kKnownXp2Build.rvaEffectHandlerDispatch);
  // changePopulation / cityCalculateYield / trackedValueAddStep 不列入致命检查：缺失时插件
  // 跳过对应 hook/注入，效果优雅降级，而不是让整个 Loader 无法初始化。
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

// 由已解析的 GameCoreApi 生成对插件可见的 EngineApi（只读函数指针集合）。
// 仅暴露“机制入口”，不含任何行为策略；proposedCombatAdjust 即战斗力修正写入点
// FUN_180944040（DB 名 GameEffects::ProposedCombat::AdjustPlayerStrengthModifier）。
bridge::EngineApi g_engineApi;
void publishEngineApi(const GameCoreApi& api) {
  bridge::EngineApi engine = {};
  engine.effectApply = api.effectApply;
  engine.effectRemove = api.effectRemove;
  engine.effectStrengthApply = api.effectStrengthApply;
  engine.effectStrengthRemove = api.effectStrengthRemove;
  engine.proposedCombatAdjust = api.strengthAccumulate;
  engine.changeYieldModifier = api.changeYieldModifier;
  engine.changePopulation = api.changePopulation;
  engine.cityCalculateYield = api.cityCalculateYield; // v7 追加
  engine.trackedValueAddStep = api.trackedValueAddStep; // v8 追加
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

  // 兜底：交由游戏 DLL 搜索路径解析标准名
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
