#include <windows.h>

#include <intrin.h> // _ReturnAddress

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include <MinHook.h>

#include "loader_internal.h"

// 效果处理器（handler）注册。
//
// 引擎对“效果运行时行为”维护两套注册：工厂表（Registry<IModifierEffectFactory>，
// 已由 registry.cpp 补齐）与处理器表（FUN_1804891b0 建立）。自定义 EffectType 只在
// 工厂表登记、未在处理器表登记时，引擎解析其行为 handler 会取到无效节点并解引用
// 0xFFFFFFFF。本文件在处理器表建立完成后，把已注册效果的类型哈希补登进去。
//
// 最小修复：直接把哈希指向模板（EFFECT_ADJUST_CITY_YIELD_MODIFIER）的 handler 对象，
// 行为等同模板。自定义 apply（每人口 × Amount%）是后续增量，需另行校准 ABI。
namespace ykkz000::loader {
namespace {

using HandlerRegistryInitFn = void* (*)(void* root);
using SetEffectHandlerFn =
    void (*)(void* root, int kind, std::uint32_t hash, void* handlerObject);
using HandlerNodeInsertFn = void* (*)(void* container, void* outNode,
                                      const std::uint32_t* hash);
using TemplateAnalyzeFn = void* (*)(void* self, void* args);
using TemplateApplyFn = std::uint64_t (*)(void* self, void* context, void* args);
#if defined(YKKZ000_ENABLE_DISPATCH_TRACE)
using HandlerDispatchFn = void* (*)(void* node);
#endif

HandlerRegistryInitFn g_originalHandlerRegistryInit = nullptr;
SetEffectHandlerFn    g_originalSetEffectHandler = nullptr;
HandlerNodeInsertFn   g_originalHandlerNodeInsert = nullptr;
TemplateAnalyzeFn     g_originalTemplateAnalyze = nullptr;
TemplateApplyFn       g_originalTemplateApply = nullptr;
#if defined(YKKZ000_ENABLE_DISPATCH_TRACE)
HandlerDispatchFn     g_originalHandlerDispatch = nullptr;
#endif
void*                 g_handlerRoot = nullptr;
// 最近一次补登所使用的 root。引擎运行时的处理器表与初始化表（g_handlerRoot）是
// 不同对象，必须跟随运行时 root 补登，否则查询命中不到我们的效果。
void*                 g_registeredRoot = nullptr;
// 仅当 SetEffectHandler_Hook 观察到“与初始化根不同的运行时根”时才置 true。
// 初始化根只在建表期有效，销毁期不可用（退出整个游戏时已释放）。
bool                  g_hasRuntimeRoot = false;
// 运行期从引擎注册表捕获的模板效果（EFFECT_ADJUST_CITY_YIELD_MODIFIER）节点。
// 不要用 profile 的数据类 RVA：那些值未经校验，登记时会把无效 handler 交给引擎。
void*                 g_templateEffectNode = nullptr;
// 我们自建的 handler 对象（handler[0] = 克隆描述表）。进程常驻、不释放：引擎注册表
// 可能在上下文拆解后仍持有它，且关停期会直接调用其 analyze/apply 槽。
void*                 g_customHandlerObject = nullptr;
// 克隆表中模板 analyze（槽 0）/ apply（槽 1）的原实现，供包装在输入有效时原样转发。
void*                 g_templateAnalyzeOriginal = nullptr;
void*                 g_templateApplyOriginal = nullptr;
// >0 表示正处于我们自己的补登调用中，用于防止 setEffectHandler hook 递归补登。
int                   g_registerDepth = 0;
bool                  g_handlerHookInstalled = false;
bool                  g_minHookInitialized = false;
std::mutex            g_handlerMutex;
std::mutex            g_hookMutex;

// hash("EFFECT_ADJUST_CITY_YIELD_MODIFIER")，与日志里的 templateHash 一致。
constexpr std::uint32_t kTemplateEffectHash = 0x1672899D;

// 诊断插桩的限流参数：前若干次全量打印，其后按步长抽样，避免人口频繁变动时刷屏。
constexpr long kTraceFullCalls = 32;
constexpr long kTraceStride = 256;

// 派发 hook 的限流参数：前 N 次全量打印，之后抽样。无效 handler 一律打印。
#if defined(YKKZ000_ENABLE_DISPATCH_TRACE)
constexpr long kDispatchFullCalls = 64;
constexpr long kDispatchStride = 256;
#endif

// 只解引用明显有效的指针，避免插桩自身在 0xFFFFFFFF 之类地址上触发二次崩溃。
bool isPlausiblePointer(const void* pointer) {
  return reinterpret_cast<std::uintptr_t>(pointer) > 0x10000;
}

// 判断 [address, address+bytes) 是否落在已提交且可读的内存区域。
// 退出整个游戏时引擎可能已先释放注册表所在的堆区，此时直接读 root+0x30 即崩，
// 因此交给引擎之前必须先做这一步校验。
bool isReadableRegion(const void* address, std::size_t bytes) {
  if (address == nullptr || bytes == 0) {
    return false;
  }
  MEMORY_BASIC_INFORMATION mbi = {};
  if (VirtualQuery(address, &mbi, sizeof(mbi)) == 0) {
    return false;
  }
  if (mbi.State != MEM_COMMIT) {
    return false;
  }
  const DWORD protection = mbi.Protect & 0xFF;
  const bool readable = protection == PAGE_READONLY || protection == PAGE_READWRITE ||
                        protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ ||
                        protection == PAGE_EXECUTE_READWRITE ||
                        protection == PAGE_EXECUTE_WRITECOPY;
  if (!readable) {
    return false;
  }
  const auto start = reinterpret_cast<std::uintptr_t>(address);
  const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
  return start + bytes <= regionEnd;
}

// FUN_1806083f0(root, 2, hash, handler) 的首批访存：root+0x08/+0x18/+0x30，
// 以及桶数组 *(root+0x18)，长度 (mask+1)*0x10，mask = *(root+0x30)。
// 任一不成立即视为陈旧 root，绝不交给引擎（退出整个游戏时 root 已释放）。
bool isPlausibleRegistryRoot(const void* root) {
  if (!isReadableRegion(root, 0x38)) {
    return false;
  }
  const auto* base = static_cast<const std::uint8_t*>(root);
  std::uint64_t mask = 0;
  std::memcpy(&mask, base + 0x30, sizeof(mask));
  void* buckets = nullptr;
  std::memcpy(&buckets, base + 0x18, sizeof(buckets));
  constexpr std::uint64_t kMaxBucketCount = 0x10000; // 防御：mask 异常即判无效
  if (mask >= kMaxBucketCount || buckets == nullptr) {
    return false;
  }
  return isReadableRegion(buckets, static_cast<std::size_t>((mask + 1) * 0x10));
}

int readInt32(const void* base, std::size_t offset) {
  int value = 0;
  std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
  return value;
}

void* readPointer(const void* base, std::size_t offset) {
  void* value = nullptr;
  std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
  return value;
}

// —— 查询/应用路径插桩：仅打印（限流），不改行为 ——
//
// 目的：判定崩溃是否位于「引擎按哈希取 handler 并应用」这条路径：
//   - set:     引擎自身与我们在 FUN_1806083f0 上的设置（用于比对 root 是否一致）；
//   - analyze: 模板描述表槽 0（FUN_18046b0d0），效果分析阶段被调用；
//   - apply:   模板描述表槽 1（FUN_18046b390），效果应用阶段被调用（决定性证据）。
std::atomic<long> s_setCalls{0};
std::atomic<long> s_analyzeCalls{0};
std::atomic<long> s_applyCalls{0};

// 把本次进程内已注册的效果补登进指定 root 的处理器表（kind=2）。幂等，可重复调用。
// 定义在本文件后部；SetEffectHandler_Hook 需要先声明。
int registerCustomEffectHandlersForRoot(void* root);

void SetEffectHandler_Hook(void* root, int kind, std::uint32_t hash, void* handlerObject) {
  const long call = ++s_setCalls;
  if (call <= kTraceFullCalls || (call % kTraceStride) == 0) {
    logMessageF(1, "set: call#%ld root=%p kind=%d hash=0x%08X handler=%p", call, root, kind,
                hash, handlerObject);
  }
  // 先让引擎自己的登记生效，再镜像补登，避免在其内部遍历期间修改同一容器。
  if (g_originalSetEffectHandler != nullptr) {
    g_originalSetEffectHandler(root, kind, hash, handlerObject);
  }

  // 引擎运行时的处理器表与初始化表是不同的 root。每当在尚未补登过的 root 上登记
  // （即运行时表首次出现），立即用同一 root 补登我们的效果；幂等且防递归。
  if (root != nullptr && g_registerDepth == 0) {
    bool needRegister = false;
    {
      std::lock_guard<std::mutex> guard(g_handlerMutex);
      if (g_registeredRoot != root) {
        g_registeredRoot = root;
        g_hasRuntimeRoot = true; // 已观察到与初始化根不同的运行时根
        needRegister = true;
      }
    }
    if (needRegister) {
      ++g_registerDepth;
      logMessageF(1, "handler: runtime root detected root=%p (from kind=%d hash=0x%08X)",
                  root, kind, hash);
      registerCustomEffectHandlersForRoot(root);
      --g_registerDepth;
    }
  }
}

// hook FUN_180489040(container, outNode, hashPtr)：handler 表插入/查找。无论插入还是
// 查找，函数都会把节点写进 outNode[0]；因此在模板效果哈希命中时捕获其节点，之后
// 读 node+0x18 即可拿到引擎真正使用的 handler 对象（登记时调用该函数取值）。
void* HandlerNodeInsert_Hook(void* container, void* outNode, const std::uint32_t* hash) {
  void* result = g_originalHandlerNodeInsert != nullptr
                     ? g_originalHandlerNodeInsert(container, outNode, hash)
                     : outNode;
  if (hash != nullptr && *hash == kTemplateEffectHash && outNode != nullptr) {
    void* node = readPointer(outNode, 0); // outNode[0] = 节点指针
    if (node != nullptr) {
      {
        std::lock_guard<std::mutex> guard(g_handlerMutex);
        g_templateEffectNode = node;
      }
      logMessageF(1, "handler: template effect node captured node=%p", node);
    }
  }
  return result;
}

// 取“引擎真正使用的”模板 handler 对象（登记时调用；此时引擎已写入 node+0x18）。
void* templateEffectHandlerObject() {
  void* node = nullptr;
  {
    std::lock_guard<std::mutex> guard(g_handlerMutex);
    node = g_templateEffectNode;
  }
  if (node != nullptr) {
    void* handler = readPointer(node, kHandlerNodeHandlerOffset); // node + 0x18
    if (isPlausiblePointer(handler)) {
      return handler;
    }
  }
  logMessage(0, "handler: template node not captured; falling back to profiled value");
  return gameCore().templateEffectHandler; // 回退（可能不正确）
}

// 关停期 context/args 可能为 0 或已失效：先校验，失效直接 no-op，绝不进入模板实现
// （模板 apply 会解引用 [x+0x81] 之类的字段，输入为 0 时即崩）。这是退出崩溃的决定性
// 修复：不能依赖 destroyGameContext 移除节点——关停路径可能绕过它直接调用 handler。
extern "C" void* ykkz000_handlerAnalyze(void* self, void* args) {
  if (!isPlausiblePointer(args)) {
    return nullptr;
  }
  return g_templateAnalyzeOriginal != nullptr
             ? reinterpret_cast<TemplateAnalyzeFn>(g_templateAnalyzeOriginal)(self, args)
             : nullptr;
}

extern "C" std::uint64_t ykkz000_handlerApply(void* self, void* context, void* args) {
  if (!isPlausiblePointer(args) || !isPlausiblePointer(context)) {
    return 0; // 关停期安全分支：不再触碰模板实现
  }
  return g_templateApplyOriginal != nullptr
             ? reinterpret_cast<TemplateApplyFn>(g_templateApplyOriginal)(self, context, args)
             : 0;
}

// 自建 handler 对象不持有引擎资源，释放即 no-op。绝不能转发到模板释放例程
// （FUN_18046b610）：它会解引用 handler 对象字段，而我们的对象不是模板对象。
extern "C" std::uint64_t ykkz000_handlerRelease(void* /*self*/, int /*flags*/) {
  return 0;
}

// 构建/复用我们的 handler 对象：克隆模板描述表，替换槽 0/1 为上面的安全包装、槽 4 为
// no-op 释放，其余槽原样复制（保持其它行为不变）。对象清零并足量分配，避免关停期引擎
// 读对象字段时取到堆垃圾。失败时回退到模板 handler（登记将不生效但不会更糟）。
void* customEffectHandlerObject() {
  if (g_customHandlerObject != nullptr) {
    return g_customHandlerObject;
  }
  void* node = nullptr;
  {
    std::lock_guard<std::mutex> guard(g_handlerMutex);
    node = g_templateEffectNode;
  }
  if (!isPlausiblePointer(node)) {
    logMessage(0, "handler: template node unavailable; falling back to template handler");
    return templateEffectHandlerObject();
  }
  void* handler = readPointer(node, kHandlerNodeHandlerOffset); // node + 0x18
  if (!isPlausiblePointer(handler)) {
    logMessage(0, "handler: template handler invalid; falling back");
    return templateEffectHandlerObject();
  }
  void* table = readPointer(handler, 0); // handler[0] = 描述表
  if (!isPlausiblePointer(table)) {
    logMessage(0, "handler: template handler table invalid; falling back");
    return templateEffectHandlerObject();
  }

  auto* clone = static_cast<void**>(
      HeapAlloc(GetProcessHeap(), 0, sizeof(void*) * kHandlerTableCloneSlots));
  if (clone == nullptr) {
    return templateEffectHandlerObject();
  }
  std::memcpy(clone, table, sizeof(void*) * kHandlerTableCloneSlots);
  g_templateAnalyzeOriginal = clone[kHandlerAnalyzeSlot];
  g_templateApplyOriginal = clone[kHandlerApplySlot];
  clone[kHandlerAnalyzeSlot] = reinterpret_cast<void*>(&ykkz000_handlerAnalyze);
  clone[kHandlerApplySlot] = reinterpret_cast<void*>(&ykkz000_handlerApply);
  clone[kHandlerReleaseSlot] = reinterpret_cast<void*>(&ykkz000_handlerRelease);

  // 清零 + 足量：handler 对象不是裸 8 字节，避免其它槽读到堆垃圾。
  auto* handlerObject = static_cast<std::uint8_t*>(
      HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, kHandlerObjectBytes));
  if (handlerObject == nullptr) {
    HeapFree(GetProcessHeap(), 0, clone);
    return templateEffectHandlerObject();
  }
  *reinterpret_cast<void**>(handlerObject) = clone; // handler[0] = 描述表
  g_customHandlerObject = handlerObject;

  logMessageF(1, "handler: custom handler cloned table=%p clone=%p analyze=%p apply=%p release=%p",
              table, clone, g_templateAnalyzeOriginal, g_templateApplyOriginal,
              reinterpret_cast<void*>(&ykkz000_handlerRelease));
  return handlerObject;
}

void* TemplateAnalyze_Hook(void* self, void* args) {
  const long call = ++s_analyzeCalls;
  if (call <= kTraceFullCalls || (call % kTraceStride) == 0) {
    logMessageF(1, "analyze: call#%ld self=%p args=%p", call, self, args);
  }
  return g_originalTemplateAnalyze != nullptr ? g_originalTemplateAnalyze(self, args) : nullptr;
}

std::uint64_t TemplateApply_Hook(void* self, void* context, void* args) {
  const long call = ++s_applyCalls;
  if (call <= kTraceFullCalls || (call % kTraceStride) == 0) {
    int amount = -1;
    int yieldType = -1;
    void* city = nullptr;
    int population = -1;
    if (isPlausiblePointer(args)) {
      amount = readInt32(args, kEffectArgsAmountOffset);
      yieldType = readInt32(args, kEffectArgsYieldTypeOffset);
    }
    if (isPlausiblePointer(context)) {
      city = readPointer(context, kEffectContextCityOffset);
      if (isPlausiblePointer(city)) {
        population = readInt32(city, kCityPopulationOffset);
      }
    }
    logMessageF(1,
                "apply: call#%ld self=%p context=%p args=%p amount=%d yield=%d city=%p pop=%d",
                call, self, context, args, amount, yieldType, city, population);
  }
  return g_originalTemplateApply != nullptr ? g_originalTemplateApply(self, context, args) : 0;
}

// handler 派发 thunk（RVA 0x979290）：
//   handler = *(node+0x18); if (handler) { table = *(handler); return table[6](handler); } return 1;
// 注意：这不是效果处理器派发，而是通用 node→对象→虚调用槽 +0x30 助手；仅作诊断用，
// 默认不安装（需要取证时用 -DYKKZ000_ENABLE_DISPATCH_TRACE=ON 重新构建）。
#if defined(YKKZ000_ENABLE_DISPATCH_TRACE)
void* HandlerDispatch_Hook(void* node) {
  static std::atomic<long> s_dispatchCalls{0};
  const long call = ++s_dispatchCalls;

  const bool nodeOk = isPlausiblePointer(node);
  const void* handler = nodeOk ? readPointer(node, kHandlerNodeHandlerOffset) : nullptr;
  const void* table = isPlausiblePointer(handler) ? readPointer(handler, 0) : nullptr;
  const void* slotSix =
      isPlausiblePointer(table) ? readPointer(table, kHandlerDispatchSlotOffset) : nullptr;
  const std::uint32_t hash =
      nodeOk ? static_cast<std::uint32_t>(readInt32(node, kHandlerNodeHashOffset)) : 0;

  // handler 非空却不可用 ⇒ 正是产生 0xFFFFFFFF 读的那类情况。
  const bool invalid =
      (handler != nullptr) &&
      (!isPlausiblePointer(handler) || !isPlausiblePointer(table) || !isPlausiblePointer(slotSix));

  if (call <= kDispatchFullCalls || (call % kDispatchStride) == 0 || invalid) {
    std::uintptr_t callerRva = 0;
    const auto moduleBase = reinterpret_cast<std::uintptr_t>(gameCore().module);
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    if (moduleBase != 0 && caller > moduleBase) {
      callerRva = caller - moduleBase;
    }
    logMessageF(invalid ? 0 : 1,
                "dispatch: call#%ld node=%p hash=0x%08X handler=%p table=%p slot6=%p "
                "caller_rva=0x%zX%s",
                call, node, hash, handler, table, slotSix, callerRva,
                invalid ? "  <-- INVALID (guard: return 1)" : "");
  }

#if !defined(YKKZ000_DISABLE_HANDLER_GUARD)
  if (invalid) {
    return reinterpret_cast<void*>(1); // 引擎自带的“无 handler”语义
  }
#endif
  return g_originalHandlerDispatch != nullptr ? g_originalHandlerDispatch(node)
                                              : reinterpret_cast<void*>(1);
}
#endif

// 把本次进程内已注册的效果补登进指定 root 的处理器表（kind=2）。返回补登条数。
int registerCustomEffectHandlersForRoot(void* root) {
  const GameCoreApi& api = gameCore();
  if (root == nullptr || api.setEffectHandler == nullptr ||
      api.templateEffectHandler == nullptr) {
    logMessage(0, "handler: registry unavailable; custom effect handlers not registered");
    return 0;
  }

  const std::vector<RegisteredEffect> effects = registeredEffects();
  int count = 0;
  for (const RegisteredEffect& effect : effects) {
    if (effect.hash == 0) {
      continue;
    }
    // 登记我们自建的 handler（克隆模板描述表 + 安全包装 analyze/apply）：关停期
    // 引擎直接调用它时，输入失效会被包装短路，不再进入模板实现而崩溃。
    void* handlerObject = customEffectHandlerObject();
    reinterpret_cast<SetEffectHandlerFn>(api.setEffectHandler)(
        root, kHandlerKindEffects, effect.hash, handlerObject);
    logMessageF(1, "handler: registered hash=0x%08X kind=%d root=%p handler=%p type=%s",
                effect.hash, kHandlerKindEffects, root, handlerObject, effect.typeName.c_str());
    ++count;
  }
  logMessageF(1, "handler: registration complete root=%p count=%d", root, count);
  return count;
}

// 移除我们在运行时表 root 上登记过的 handler 节点（handlerObject=nullptr ⇒ 引擎移除该节点）。
// 只在真实 DllDestroyGameContext 之前调用。退出整个游戏时引擎可能已先拆掉注册表，
// 此时保存的 root 内存不可读：必须先用 isPlausibleRegistryRoot 校验，绝不能再交给
// 引擎（否则 FUN_1806083f0 读 root+0x30 即崩，见 exit-crash 记录 #2）。
void removeCustomEffectHandlersImpl() {
  const GameCoreApi& api = gameCore();
  if (api.setEffectHandler == nullptr) {
    return;
  }
  void* root = nullptr;
  {
    std::lock_guard<std::mutex> guard(g_handlerMutex);
    if (g_hasRuntimeRoot) {
      root = g_registeredRoot; // 只用运行时表 root；初始化根不作为销毁期目标
    }
  }
  if (root == nullptr) {
    logMessage(1, "handler: no runtime root recorded; skip removal");
    return;
  }
  if (!isPlausibleRegistryRoot(root)) {
    // 退出整个游戏时引擎可能已先拆掉注册表：此时内存不可读，直接跳过。
    // 槽 4 已是 no-op，跳过移除不会导致关停期崩溃。
    logMessageF(1, "handler: skip stale root %p; removal aborted", root);
    return;
  }
  const std::vector<RegisteredEffect> effects = registeredEffects();
  int removed = 0;
  for (const RegisteredEffect& effect : effects) {
    if (effect.hash == 0) {
      continue;
    }
    reinterpret_cast<SetEffectHandlerFn>(api.setEffectHandler)(
        root, kHandlerKindEffects, effect.hash, nullptr);
    ++removed;
  }
  logMessageF(1, "handler: custom handlers removed root=%p count=%d", root, removed);
}

// hook FUN_1804891b0：RCX 即处理器表根。必须先让原函数建好全部内建节点，再把我们的
// 效果补登进去；每个游戏上下文都会重建根对象，因此每次都要重新登记。
//
// 注：反编译确认原函数以 RAX 回传其入参 root，故这里保持 void* 返回并回传跳板结果。
void* HandlerRegistryInit_Hook(void* root) {
  if (g_originalHandlerRegistryInit == nullptr) {
    return root;
  }
  logMessageF(1, "registry-init: root=%p", root);
  {
    std::lock_guard<std::mutex> guard(g_handlerMutex);
    g_handlerRoot = root;
    g_registeredRoot = root; // 初始化表已补登
  }
  void* result = g_originalHandlerRegistryInit(root);
  registerCustomEffectHandlersForRoot(root);
  return result;
}

} // namespace

void removeCustomEffectHandlers() {
  removeCustomEffectHandlersImpl();
}

bool installEffectHandlerHook() {
#if defined(YKKZ000_DISABLE_EFFECT_HANDLER)
  logMessage(1, "Effect handler hook: disabled by YKKZ000_DISABLE_EFFECT_HANDLER");
  return false;
#else
  std::lock_guard<std::mutex> guard(g_hookMutex);
  if (g_handlerHookInstalled) {
    return true;
  }
  LogScope scope("install effect handler hook");
  const GameCoreApi& api = gameCore();
  logMessageF(1, "handler: registryInit=%p setHandler=%p template=%p table=%p analyze=%p apply=%p",
              api.handlerRegistryInit, api.setEffectHandler, api.templateEffectHandler,
              api.templateHandlerTable, api.templateAnalyze, api.templateApply);
  if (api.handlerRegistryInit == nullptr || api.setEffectHandler == nullptr) {
    logMessage(0, "Effect handler hook: entry points unavailable; custom effects may crash "
                  "when the engine resolves their handler");
    return false;
  }
  if (!g_minHookInitialized) {
    const MH_STATUS status = MH_Initialize();
    logMessageF(1, "handler: MH_Initialize -> %d", static_cast<int>(status));
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
      logMessage(0, "Effect handler hook: MinHook initialization failed");
      return false;
    }
    g_minHookInitialized = true;
  }
  if (g_originalHandlerRegistryInit == nullptr) {
    const MH_STATUS created = MH_CreateHook(
        api.handlerRegistryInit, reinterpret_cast<LPVOID>(&HandlerRegistryInit_Hook),
        reinterpret_cast<LPVOID*>(&g_originalHandlerRegistryInit));
    logMessageF(1, "handler: MH_CreateHook -> %d trampoline=%p", static_cast<int>(created),
                g_originalHandlerRegistryInit);
    if (created != MH_OK) {
      logMessage(0, "Effect handler hook: MinHook CreateHook failed");
      return false;
    }
  }
  const MH_STATUS enabled = MH_EnableHook(api.handlerRegistryInit);
  logMessageF(1, "handler: MH_EnableHook -> %d", static_cast<int>(enabled));
  if (enabled != MH_OK) {
    logMessage(0, "Effect handler hook: MinHook EnableHook failed");
    return false;
  }

  // 诊断插桩（只打印，不改行为）：确认引擎是否把自定义效果走到 handler 应用路径。
  // 失败不视为致命——补登本身不依赖这三处 hook。
  auto addTraceHook = [](void* target, LPVOID detour, LPVOID* trampoline,
                         const char* name) {
    if (target == nullptr) {
      logMessageF(1, "handler: trace hook %s target unavailable; skipped", name);
      return;
    }
    if (*trampoline == nullptr) {
      const MH_STATUS created = MH_CreateHook(target, detour, trampoline);
      if (created != MH_OK) {
        logMessageF(0, "handler: MH_CreateHook(%s) -> %d", name, static_cast<int>(created));
        return;
      }
      logMessageF(1, "handler: trace hook %s created target=%p trampoline=%p", name, target,
                  *trampoline);
    }
    const MH_STATUS hookEnabled = MH_EnableHook(target);
    if (hookEnabled != MH_OK) {
      logMessageF(0, "handler: MH_EnableHook(%s) -> %d", name, static_cast<int>(hookEnabled));
      return;
    }
    logMessageF(1, "handler: trace hook %s enabled target=%p", name, target);
  };
  addTraceHook(api.setEffectHandler, reinterpret_cast<LPVOID>(&SetEffectHandler_Hook),
               reinterpret_cast<LPVOID*>(&g_originalSetEffectHandler), "setEffectHandler");
  addTraceHook(api.handlerNodeInsert, reinterpret_cast<LPVOID>(&HandlerNodeInsert_Hook),
               reinterpret_cast<LPVOID*>(&g_originalHandlerNodeInsert), "handlerNodeInsert");
  addTraceHook(api.templateAnalyze, reinterpret_cast<LPVOID>(&TemplateAnalyze_Hook),
               reinterpret_cast<LPVOID*>(&g_originalTemplateAnalyze), "templateAnalyze");
  addTraceHook(api.templateApply, reinterpret_cast<LPVOID>(&TemplateApply_Hook),
               reinterpret_cast<LPVOID*>(&g_originalTemplateApply), "templateApply");
#if defined(YKKZ000_ENABLE_DISPATCH_TRACE)
  addTraceHook(api.effectHandlerDispatch, reinterpret_cast<LPVOID>(&HandlerDispatch_Hook),
               reinterpret_cast<LPVOID*>(&g_originalHandlerDispatch), "handlerDispatch");
#endif

  g_handlerHookInstalled = true;
  logMessage(1, "Effect handler hook: installed");
  return true;
#endif
}

// 反安装只停用 hook，不释放 MinHook 跳板（与 population hook 同一策略：反安装时可能
// 仍有线程执行在 detour 内，释放跳板会调用到已释放内存）。下一个游戏上下文由
// installEffectHandlerHook() 重新启用。
void uninstallEffectHandlerHook() {
  std::lock_guard<std::mutex> guard(g_hookMutex);
  LogScope scope("uninstall effect handler hook");
  if (g_handlerHookInstalled) {
    const GameCoreApi& api = gameCore();
    if (api.handlerRegistryInit != nullptr) {
      MH_DisableHook(api.handlerRegistryInit);
    }
    if (api.setEffectHandler != nullptr) {
      MH_DisableHook(api.setEffectHandler);
    }
    if (api.handlerNodeInsert != nullptr) {
      MH_DisableHook(api.handlerNodeInsert);
    }
    if (api.templateAnalyze != nullptr) {
      MH_DisableHook(api.templateAnalyze);
    }
    if (api.templateApply != nullptr) {
      MH_DisableHook(api.templateApply);
    }
#if defined(YKKZ000_ENABLE_DISPATCH_TRACE)
    if (api.effectHandlerDispatch != nullptr) {
      MH_DisableHook(api.effectHandlerDispatch);
    }
#endif
    g_handlerHookInstalled = false;
  }
  {
    std::lock_guard<std::mutex> rootGuard(g_handlerMutex);
    g_handlerRoot = nullptr;
    g_registeredRoot = nullptr;
    g_hasRuntimeRoot = false;
    g_templateEffectNode = nullptr;
    g_registerDepth = 0;
  }
  // 旧克隆与旧 handler 对象不释放（引擎注册表可能仍引用，须进程常驻）；仅清缓存，
  // 使下一个上下文用新捕获的模板节点重建。
  g_customHandlerObject = nullptr;
  g_templateAnalyzeOriginal = nullptr;
  g_templateApplyOriginal = nullptr;
}

} // namespace ykkz000::loader
