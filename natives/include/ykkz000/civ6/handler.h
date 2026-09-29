#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ykkz000::civ6 {

// 效果处理器（handler）的描述表：一个函数指针数组。
// 槽 0 = analyze、槽 1 = apply（由模板实现与运行期日志确认）；槽 6（+0x30）由派发
// thunk 0x979290 尾调用（rcx=[rcx+0x18]; jmp [rax+0x30]）。
struct HandlerTable {
  void* (*analyze)(void* self, void* args);                     // 0x00: 槽 0
  std::uint64_t (*apply)(void* self, void* context, void* args); // 0x08: 槽 1
  void* unknown_0x10;                                           // 0x10: 槽 2（未验证）
  void* unknown_0x18;                                           // 0x18: 槽 3（未验证）
  std::uint64_t (*release)(void* self, int flags);              // 0x20: 槽 4
  void* unknown_0x28;                                           // 0x28: 槽 5（未验证）
  void* (*dispatch_slot)(void* handler);                        // 0x30: 槽 6
};

static_assert(std::is_standard_layout_v<HandlerTable>);
static_assert(offsetof(HandlerTable, analyze) == 0x00);
static_assert(offsetof(HandlerTable, apply) == 0x08);
static_assert(offsetof(HandlerTable, release) == 0x20);
static_assert(offsetof(HandlerTable, dispatch_slot) == 0x30);
static_assert(sizeof(HandlerTable) == 0x38);

// 描述表槽位（由字段偏移导出，保证与布局一致）。
inline constexpr std::size_t kHandlerAnalyzeSlot = offsetof(HandlerTable, analyze) / sizeof(void*);
inline constexpr std::size_t kHandlerApplySlot = offsetof(HandlerTable, apply) / sizeof(void*);
inline constexpr std::size_t kHandlerReleaseSlot = offsetof(HandlerTable, release) / sizeof(void*);

static_assert(kHandlerAnalyzeSlot == 0);
static_assert(kHandlerApplySlot == 1);
static_assert(kHandlerReleaseSlot == 4);

// handler 对象为单指针对象：handler[0] 指向描述表。自建对象必须清零并足量分配
// （0x40 字节），否则其它槽会读到堆垃圾（退出崩溃根因）。
struct HandlerObject {
  HandlerTable* table;             // 0x00: 描述表
  std::uint8_t unknown_0x08[0x38]; // 0x08..0x3F: 未知（模板对象字段）
};

static_assert(std::is_standard_layout_v<HandlerObject>);
static_assert(offsetof(HandlerObject, table) == 0x00);
static_assert(sizeof(HandlerObject) == 0x40);

// 引擎“处理器（handler）”哈希表节点（0x20 字节）。
struct HandlerNode {
  HandlerNode* next;            // 0x00: 链下一节点
  HandlerNode* prev;            // 0x08: 链上一节点
  std::uint32_t hash;           // 0x10: EffectType 哈希
  std::uint8_t unknown_0x14[4]; // 0x14: 未知（对齐填充）
  HandlerObject* handler;       // 0x18: handler 对象
};

static_assert(std::is_standard_layout_v<HandlerNode>);
static_assert(offsetof(HandlerNode, hash) == 0x10);
static_assert(offsetof(HandlerNode, handler) == 0x18);
static_assert(sizeof(HandlerNode) == 0x20);

// 处理器哈希表的表头（FUN_180489040 现场）：桶数组 = buckets，长度 = (mask+1)*0x10。
// 表头即为注册表根的每个 kind 条目（见 HandlerRegistryRoot）。
struct HandlerHashTable {  std::uint8_t unknown_0x00[0x18]; // 0x00..0x17: 未知
  void* buckets;                   // 0x18: 桶数组
  std::uint8_t unknown_0x20[0x10]; // 0x20..0x2F: 未知
  std::uint64_t mask;              // 0x30: 桶数掩码
  std::uint8_t unknown_0x38[0x8];  // 0x38..0x3F: 未知（对齐填充）
};

static_assert(std::is_standard_layout_v<HandlerHashTable>);
static_assert(offsetof(HandlerHashTable, buckets) == 0x18);
static_assert(offsetof(HandlerHashTable, mask) == 0x30);
static_assert(sizeof(HandlerHashTable) == 0x40);

// 哈希表桶数组的元素大小（FUN_1806083f0 现场：桶数组长度 = (mask+1)*0x10）。
inline constexpr std::size_t kHandlerBucketBytes = 0x10;

// 处理器注册表根（FUN_1804891b0(root) 建立）：每个 kind 一个哈希表。
//   +0x00 效果表（kind=2）、+0x40 集合表（kind=3）、+0x80 需求表。
// 引擎运行时的注册表与初始化时的注册表是不同对象，销毁期必须先校验根内存可读。
struct HandlerRegistryRoot {
  HandlerHashTable effects;      // 0x00: 效果表（kind=2）
  HandlerHashTable collections;  // 0x40: 集合表（kind=3）
  HandlerHashTable requirements; // 0x80: 需求表
};

static_assert(std::is_standard_layout_v<HandlerRegistryRoot>);
static_assert(offsetof(HandlerRegistryRoot, effects) == 0x00);
static_assert(offsetof(HandlerRegistryRoot, collections) == 0x40);
static_assert(offsetof(HandlerRegistryRoot, requirements) == 0x80);
static_assert(sizeof(HandlerRegistryRoot) == 0xc0);

} // namespace ykkz000::civ6
