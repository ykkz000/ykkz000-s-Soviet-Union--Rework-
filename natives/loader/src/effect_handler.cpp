#include <windows.h>

#include <intrin.h> // _ReturnAddress

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "loader_internal.h"

// 效果处理器（handler）注册。
//
// 引擎对“效果运行时行为”维护两套注册：工厂表（Registry<IModifierEffectFactory>，
// 已由 registry.cpp 补齐）与处理器表（FUN_1804891b0 建立）。自定义 EffectType 只在
// 工厂表登记、未在处理器表登记时，引擎解析其行为 handler 会取到无效节点并解引用
// 0xFFFFFFFF。本文件在处理器表建立完成后，把已注册效果的类型哈希补登进去。
//
// 每个自定义效果克隆其模板 handler 的描述表，槽 0/1 指向本文件的安全包装；包装在
// 输入有效且插件提供了 analyze/handlerApply 时转发给插件（SEH 包裹），否则转发模板。
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
// 不同对象，必须跟随最新 root 补登，否则查询命中不到我们的效果。
void*                 g_registeredRoot = nullptr;

// 多模板支持：每个自定义效果复用自己的模板效果（如城市产出修正 / 玩家战斗力修正）。
// 运行期从引擎注册表按模板哈希分别捕获节点，再为每个效果克隆各自的描述表与
// handler 对象，避免把 A 模板的 apply 用到 B 效果上。
//
// 说明：模板 handler 一律运行期捕获，不用 profile 里的数据类 RVA（多次运行对不上）。
std::unordered_map<std::uint32_t, void*> g_templateNodes;         // templateHash -> 节点
std::unordered_map<std::uint32_t, void*> g_customHandlerObjects;  // effectHash -> 自建对象
// 自建 handler 对象 -> 其克隆表中模板 analyze/apply 的原实现（包装转发用），以及
// 插件提供的替换回调与其归属句柄。键必须是 handler 对象本身；handler[0] 才是描述表。
struct HandlerOriginals {
  void* analyze = nullptr;
  void* apply = nullptr;
  bridge::AnalyzeFn pluginAnalyze = nullptr;
  bridge::ApplyFn pluginApply = nullptr;
  void* pluginHandle = nullptr;
};
std::unordered_map<void*, HandlerOriginals> g_handlerOriginals;

// >0 表示正处于我们自己的补登调用中，用于防止 setEffectHandler hook 递归补登。
int                   g_registerDepth = 0;
bool                  g_handlerHookInstalled = false;
std::mutex            g_handlerMutex;
std::mutex            g_hookMutex;

// 构建档案中唯一带有可用 handler 数据地址的模板效果（hash 为
// "EFFECT_ADJUST_CITY_YIELD_MODIFIER"）。仅用于“模板节点尚未被捕获时回退到档案
// 数据地址”这一条机制退路；Loader 不因此认识任何行为。
constexpr std::uint32_t kProfiledTemplateHash = 0x1672899D;

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

// FUN_1806083f0(root, 2, hash, handler) 的首批访存：root+0x08/+0x18/+0x30，
// 以及桶数组 *(root+0x18)，长度 (mask+1)*0x10，mask = *(root+0x30)。
// 任一不成立即视为陈旧 root，绝不交给引擎（退出整个游戏时 root 已释放）。
bool isPlausibleRegistryRoot(const void* root) {
  // root 的 +0x00 即效果表（kind=2）的哈希表头（见 civ6::HandlerRegistryRoot）。
  constexpr std::size_t kMaskEnd =
      offsetof(civ6::HandlerHashTable, mask) + sizeof(std::uint64_t);
  if (!isReadableRegion(root, kMaskEnd)) {
    return false;
  }
  const std::size_t tableOffset = offsetof(civ6::HandlerRegistryRoot, effects);
  std::uint64_t mask = 0;
  (void)tryReadField(root, tableOffset + offsetof(civ6::HandlerHashTable, mask), mask);
  void* buckets = nullptr;
  (void)tryReadField(root, tableOffset + offsetof(civ6::HandlerHashTable, buckets), buckets);
  constexpr std::uint64_t kMaxBucketCount = 0x10000; // 防御：mask 异常即判无效
  if (mask >= kMaxBucketCount || buckets == nullptr) {
    return false;
  }
  return isReadableRegion(buckets, static_cast<std::size_t>((mask + 1) * civ6::kHandlerBucketBytes));
}

// 只在校验可读后取值；失败返回 fallback。插桩与节点解析共用，避免在不可读地址上崩溃。
int readInt32(const void* base, std::size_t offset, int fallback = 0) {
  int value = fallback;
  (void)tryReadField(base, offset, value);
  return value;
}

void* readPointer(const void* base, std::size_t offset) {
  void* value = nullptr;
  (void)tryReadField(base, offset, value);
  return value;
}

// —— SEH 包裹的插件回调 ——
// MSVC 禁止在含需要栈展开的 C++ 对象的函数里使用 __try，故这里保持 POD-only 的
// 薄包装；VEH 见到 g_guardedCallActive 时直接放行，交给 __except 接管（不写崩溃日志）。
void* callGuardedAnalyze(bridge::AnalyzeFn fn, void* self, void* args) {
  g_guardedCallActive = true;
  void* result = nullptr;
  __try {
    result = fn(self, args);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    result = nullptr;
  }
  g_guardedCallActive = false;
  return result;
}

std::uint64_t callGuardedApply(bridge::ApplyFn fn, void* self, void* a1, void* a2, void* a3) {
  g_guardedCallActive = true;
  std::uint64_t result = 0;
  __try {
    result = fn(self, a1, a2, a3);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    result = 0;
  }
  g_guardedCallActive = false;
  return result;
}

// —— 查询/应用路径插桩：仅打印（限流），不改行为 ——
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
// 查找，函数都会把节点写进 outNode[0]；因此在任一“已注册模板哈希”命中时捕获其节点，
// 之后读 node+0x18 即可拿到引擎真正使用的 handler 对象（登记时调用该函数取值）。
void* HandlerNodeInsert_Hook(void* container, void* outNode, const std::uint32_t* hash) {
  void* result = g_originalHandlerNodeInsert != nullptr
                     ? g_originalHandlerNodeInsert(container, outNode, hash)
                     : outNode;
  if (hash != nullptr && outNode != nullptr && isRegisteredTemplateHash(*hash)) {
    void* node = readPointer(outNode, 0); // outNode[0] = 节点指针
    if (node != nullptr) {
      {
        std::lock_guard<std::mutex> guard(g_handlerMutex);
        g_templateNodes[*hash] = node;
      }
      logMessageF(1, "handler: template effect node captured template=0x%08X node=%p", *hash,
                  node);
    }
  }
  return result;
}

// 按模板哈希回退到 profile 记录的数据地址。仅城市产出模板保留该退路（历史兼容）；
// 其它模板无退路。
void* profiledTemplateHandler(std::uint32_t templateHash) {
  if (templateHash == kProfiledTemplateHash) {
    logMessage(0, "handler: profiled template node not captured; falling back to profiled value");
    return gameCore().templateEffectHandler; // 回退（可能不正确）
  }
  return nullptr;
}

// 取“引擎真正使用的”模板 handler 对象（登记时调用；此时引擎已写入 node+0x18）。
void* templateEffectHandlerObject(std::uint32_t templateHash) {
  void* node = nullptr;
  {
    std::lock_guard<std::mutex> guard(g_handlerMutex);
    const auto it = g_templateNodes.find(templateHash);
    if (it != g_templateNodes.end()) {
      node = it->second;
    }
  }
  if (isPlausiblePointer(node)) {
    void* handler = readPointer(node, offsetof(civ6::HandlerNode, handler)); // node + 0x18
    if (isPlausiblePointer(handler)) {
      return handler;
    }
  }
  void* fallback = profiledTemplateHandler(templateHash);
  if (fallback != nullptr) {
    return fallback;
  }
  logMessageF(0, "handler: template node 0x%08X not captured; handler unavailable", templateHash);
  return nullptr;
}

// 关停期 context/args 可能为 0 或已失效：先校验，失效直接 no-op，绝不进入模板实现
// （模板 apply 会解引用 [x+0x81] 之类的字段，输入为 0 时即崩）。这是退出崩溃的决定性
// 修复：不能依赖 destroyGameContext 移除节点——关停路径可能绕过它直接调用 handler。
// 通过 self（handler 对象）查其模板原始槽并转发；self 是 handler 对象，handler[0]
// 才是描述表，绝不能拿描述表当键。插件回调存在时优先转发插件（SEH 包裹）。
extern "C" void* ykkz000_handlerAnalyze(void* self, void* args) {
  if (!isPlausiblePointer(args)) {
    return nullptr;
  }
  HandlerOriginals originals;
  {
    std::lock_guard<std::mutex> guard(g_handlerMutex);
    const auto it = g_handlerOriginals.find(self);
    if (it != g_handlerOriginals.end()) {
      originals = it->second;
    }
  }
  if (originals.pluginAnalyze != nullptr) {
    return callGuardedAnalyze(originals.pluginAnalyze, self, args);
  }
  return originals.analyze != nullptr
             ? reinterpret_cast<TemplateAnalyzeFn>(originals.analyze)(self, args)
             : nullptr;
}

extern "C" std::uint64_t ykkz000_handlerApply(void* self, void* context, void* args) {
  if (!isPlausiblePointer(args) || !isPlausiblePointer(context)) {
    return 0; // 关停期安全分支：不再触碰模板/插件实现
  }
  HandlerOriginals originals;
  {
    std::lock_guard<std::mutex> guard(g_handlerMutex);
    const auto it = g_handlerOriginals.find(self);
    if (it != g_handlerOriginals.end()) {
      originals = it->second;
    }
  }
  if (originals.pluginApply != nullptr) {
    return callGuardedApply(originals.pluginApply, self, context, args, nullptr);
  }
  return originals.apply != nullptr
             ? reinterpret_cast<TemplateApplyFn>(originals.apply)(self, context, args)
             : 0;
}

// 自建 handler 对象不持有引擎资源，释放即 no-op。绝不能转发到模板释放例程
// （FUN_18046b610）：它会解引用 handler 对象字段，而我们的对象不是模板对象。
extern "C" std::uint64_t ykkz000_handlerRelease(void* /*self*/, int /*flags*/) {
  return 0;
}

// 构建/复用指定效果的 handler 对象：克隆模板描述表，替换槽 0/1 为上面的安全包装、
// 槽 4 为 no-op 释放，其余槽原样复制（保持其它行为不变）。对象清零并足量分配，避免
// 关停期引擎读对象字段时取到堆垃圾。失败返回 nullptr（调用方跳过登记，绝不登记空
// handler 让引擎解引用无效节点）。
void* customEffectHandlerObject(std::uint32_t effectHash, std::uint32_t templateHash,
                               const bridge::EffectImpl& impl, void* pluginHandle) {
  {
    std::lock_guard<std::mutex> guard(g_handlerMutex);
    const auto it = g_customHandlerObjects.find(effectHash);
    if (it != g_customHandlerObjects.end()) {
      return it->second;
    }
  }
  void* handler = templateEffectHandlerObject(templateHash);
  if (!isPlausiblePointer(handler)) {
    logMessageF(0, "handler: template 0x%08X handler unavailable; clone aborted", templateHash);
    return nullptr;
  }
  void* table = readPointer(handler, offsetof(civ6::HandlerObject, table)); // handler[0] = 描述表
  if (!isPlausiblePointer(table)) {
    logMessageF(0, "handler: template 0x%08X handler table invalid; clone aborted", templateHash);
    return nullptr;
  }

  auto* clone = static_cast<void**>(
      HeapAlloc(GetProcessHeap(), 0, sizeof(void*) * kHandlerTableCloneSlots));
  if (clone == nullptr) {
    return nullptr;
  }
  std::memcpy(clone, table, sizeof(void*) * kHandlerTableCloneSlots);
  HandlerOriginals originals;
  originals.analyze = clone[civ6::kHandlerAnalyzeSlot];
  originals.apply = clone[civ6::kHandlerApplySlot];
  originals.pluginAnalyze = impl.analyze;
  originals.pluginApply = impl.handlerApply;
  originals.pluginHandle = pluginHandle;
  clone[civ6::kHandlerAnalyzeSlot] = reinterpret_cast<void*>(&ykkz000_handlerAnalyze);
  clone[civ6::kHandlerApplySlot] = reinterpret_cast<void*>(&ykkz000_handlerApply);
  clone[civ6::kHandlerReleaseSlot] = reinterpret_cast<void*>(&ykkz000_handlerRelease);

  // 清零 + 足量：handler 对象不是裸 8 字节，避免其它槽读到堆垃圾。
  auto* handlerObject = static_cast<std::uint8_t*>(
      HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(civ6::HandlerObject)));
  if (handlerObject == nullptr) {
    HeapFree(GetProcessHeap(), 0, clone);
    return nullptr;
  }
  (void)TryWrite(handlerObject, &civ6::HandlerObject::table,
                 reinterpret_cast<civ6::HandlerTable*>(clone));

  {
    std::lock_guard<std::mutex> guard(g_handlerMutex);
    g_handlerOriginals[handlerObject] = originals;
    g_customHandlerObjects[effectHash] = handlerObject;
  }

  logMessageF(1,
              "handler: custom handler cloned effect=0x%08X template=0x%08X table=%p clone=%p "
              "analyze=%p apply=%p release=%p plugin=%p/%p",
              effectHash, templateHash, table, clone, originals.analyze, originals.apply,
              reinterpret_cast<void*>(&ykkz000_handlerRelease),
              reinterpret_cast<void*>(originals.pluginAnalyze),
              reinterpret_cast<void*>(originals.pluginApply));
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
      amount = readInt32(args, offsetof(civ6::EffectArgs, amount));
      yieldType = readInt32(args, offsetof(civ6::EffectArgs, yield_type));
    }
    if (isPlausiblePointer(context)) {
      city = readPointer(context, offsetof(civ6::EffectContext, city));
      if (isPlausiblePointer(city)) {
        population = readInt32(city, offsetof(civ6::City::Instance, population));
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
  const void* handler = nodeOk ? readPointer(node, offsetof(civ6::HandlerNode, handler)) : nullptr;
  const void* table =
      isPlausiblePointer(handler) ? readPointer(handler, offsetof(civ6::HandlerObject, table))
                                  : nullptr;
  const void* slotSix =
      isPlausiblePointer(table) ? readPointer(table, offsetof(civ6::HandlerTable, dispatch_slot))
                                : nullptr;
  const std::uint32_t hash =
      nodeOk ? static_cast<std::uint32_t>(readInt32(node, offsetof(civ6::HandlerNode, hash))) : 0;

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
  if (root == nullptr || api.setEffectHandler == nullptr) {
    logMessage(0, "handler: registry unavailable; custom effect handlers not registered");
    return 0;
  }

  const std::vector<RegisteredEffect> effects = registeredEffects();
  int count = 0;
  for (const RegisteredEffect& effect : effects) {
    if (effect.hash == 0) {
      continue;
    }
    EffectRecord record;
    (void)findEffectRecord(effect.hash, record); // 未登记实现则视为纯模板复用
    // 登记该效果自建 handler（克隆模板描述表 + 安全包装 analyze/apply）：关停期
    // 引擎直接调用它时，输入失效会被包装短路，不再进入模板实现而崩溃。
    void* handlerObject =
        customEffectHandlerObject(effect.hash, effect.templateHash, record.impl,
                                  record.pluginHandle);
    if (handlerObject == nullptr) {
      logMessageF(0, "handler: no handler for hash=0x%08X template=0x%08X; skipped",
                  effect.hash, effect.templateHash);
      continue;
    }
    reinterpret_cast<SetEffectHandlerFn>(api.setEffectHandler)(
        root, kHandlerKindEffects, effect.hash, handlerObject);
    logMessageF(1,
                "handler: registered hash=0x%08X kind=%d root=%p handler=%p template=0x%08X "
                "type=%s",
                effect.hash, kHandlerKindEffects, root, handlerObject, effect.templateHash,
                effect.typeName.c_str());
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
    root = g_registeredRoot; // 初始化根只在建表期有效；不可读时由下方的校验拦下
  }
  if (root == nullptr) {
    logMessage(1, "handler: no registry root recorded; skip removal");
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

void clearHandlerCallbacksForPlugin(void* pluginHandle) {
  if (pluginHandle == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> guard(g_handlerMutex);
  for (auto& pair : g_handlerOriginals) {
    HandlerOriginals& originals = pair.second;
    if (originals.pluginHandle != pluginHandle) {
      continue;
    }
    originals.pluginAnalyze = nullptr;
    originals.pluginApply = nullptr;
    originals.pluginHandle = nullptr;
  }
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
  if (!ensureHookServiceInitialized()) {
    logMessage(0, "Effect handler hook: MinHook initialization failed");
    return false;
  }

  auto addHook = [](void* target, LPVOID detour, LPVOID* trampoline, const char* name) {
    if (target == nullptr) {
      logMessageF(1, "handler: hook %s target unavailable; skipped", name);
      return;
    }
    const int status = installHookRaw(target, detour, reinterpret_cast<void**>(trampoline));
    if (status != 0) {
      logMessageF(0, "handler: install hook %s -> %d", name, status);
      return;
    }
    logMessageF(1, "handler: hook %s installed target=%p trampoline=%p", name, target, *trampoline);
  };

  addHook(api.handlerRegistryInit, reinterpret_cast<LPVOID>(&HandlerRegistryInit_Hook),
          reinterpret_cast<LPVOID*>(&g_originalHandlerRegistryInit), "handlerRegistryInit");

  // 诊断插桩（只打印，不改行为）：确认引擎是否把自定义效果走到 handler 应用路径。
  // 失败不视为致命——补登本身不依赖这三处 hook。
  addHook(api.setEffectHandler, reinterpret_cast<LPVOID>(&SetEffectHandler_Hook),
          reinterpret_cast<LPVOID*>(&g_originalSetEffectHandler), "setEffectHandler");
  addHook(api.handlerNodeInsert, reinterpret_cast<LPVOID>(&HandlerNodeInsert_Hook),
          reinterpret_cast<LPVOID*>(&g_originalHandlerNodeInsert), "handlerNodeInsert");
  addHook(api.templateAnalyze, reinterpret_cast<LPVOID>(&TemplateAnalyze_Hook),
          reinterpret_cast<LPVOID*>(&g_originalTemplateAnalyze), "templateAnalyze");
  addHook(api.templateApply, reinterpret_cast<LPVOID>(&TemplateApply_Hook),
          reinterpret_cast<LPVOID*>(&g_originalTemplateApply), "templateApply");
#if defined(YKKZ000_ENABLE_DISPATCH_TRACE)
  addHook(api.effectHandlerDispatch, reinterpret_cast<LPVOID>(&HandlerDispatch_Hook),
          reinterpret_cast<LPVOID*>(&g_originalHandlerDispatch), "handlerDispatch");
#endif

  g_handlerHookInstalled = true;
  logMessage(1, "Effect handler hook: installed");
  return true;
#endif
}

// 反安装只停用 hook，不释放 MinHook 跳板（与其它 hook 同一策略：反安装时可能仍有
// 线程执行在 detour 内，释放跳板会调用到已释放内存）。下一个游戏上下文由
// installEffectHandlerHook() 重新启用。
void uninstallEffectHandlerHook() {
  std::lock_guard<std::mutex> guard(g_hookMutex);
  LogScope scope("uninstall effect handler hook");
  if (g_handlerHookInstalled) {
    const GameCoreApi& api = gameCore();
    if (api.handlerRegistryInit != nullptr) {
      (void)removeHookRaw(api.handlerRegistryInit);
    }
    if (api.setEffectHandler != nullptr) {
      (void)removeHookRaw(api.setEffectHandler);
    }
    if (api.handlerNodeInsert != nullptr) {
      (void)removeHookRaw(api.handlerNodeInsert);
    }
    if (api.templateAnalyze != nullptr) {
      (void)removeHookRaw(api.templateAnalyze);
    }
    if (api.templateApply != nullptr) {
      (void)removeHookRaw(api.templateApply);
    }
#if defined(YKKZ000_ENABLE_DISPATCH_TRACE)
    if (api.effectHandlerDispatch != nullptr) {
      (void)removeHookRaw(api.effectHandlerDispatch);
    }
#endif
    g_handlerHookInstalled = false;
  }
  {
    std::lock_guard<std::mutex> rootGuard(g_handlerMutex);
    g_handlerRoot = nullptr;
    g_registeredRoot = nullptr;
    g_registerDepth = 0;
    // 旧克隆、旧 handler 对象与原始槽映射不释放（引擎注册表可能仍引用，须进程常驻）；
    // 仅清缓存，使下一个上下文用新捕获的模板节点重建。清空原始槽映射后，若关停期仍有
    // 线程调用旧 handler 的安全包装，会安全地 no-op 而非转发到已释放的模板实现。
    g_templateNodes.clear();
    g_customHandlerObjects.clear();
    g_handlerOriginals.clear();
  }
}

} // namespace ykkz000::loader
