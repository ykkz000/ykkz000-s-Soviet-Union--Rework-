#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

// GameCore 引擎运行时内存布局（由 GameCore_XP2_FinalRelease.dll 逆向确认）。
//
// 本目录只描述布局：类型仅含 POD 数据成员（void*、定长整数、定长数组），不含
// 虚函数、基类与非平凡成员，也不调用任何引擎函数；每个已知字段以 static_assert
// 锁定偏移与大小。loader 侧的硬编码偏移与 vtable 槽号必须改为引用此处的类型
// （offsetof / sizeof / 槽位常量），使引擎布局只存在一个事实来源。
namespace ykkz000::civ6 {

// —— MSVC vtable 前置槽 ——
// vtable[-1] 为 RTTI/COL 指针，克隆 vtable 时必须一并复制。否则新 vptr 前方是
// HeapAlloc 块头，引擎的 dynamic_cast/typeid/异常展开会把堆头当指针用而写坏内存。
inline constexpr std::size_t kVTableRttiPrefixSlots = 1;

// IModifierEffectFactory 工厂对象的 vtable 宽度与关键槽位。
// 该接口宽于 6 槽：实测会在槽 6（+0x30）上分发，克隆必须“足够宽且完整复制”，
// 绝不能只复制 6 个，否则越界读到堆垃圾并跳到 0xFFFFFFFF。
inline constexpr std::size_t kFactoryVTableSlots = 24;
inline constexpr std::size_t kFactoryTypeIdSlot = 1;
inline constexpr std::size_t kFactoryTypeNameSlot = 2;
inline constexpr std::size_t kFactoryCreateSlot = 5;

// std::vector 三指针视图（MSVC 布局）。
template <typename T>
struct VectorView {
  T* begin;    // 0x00: 首元素
  T* end;      // 0x08: 尾后
  T* capacity; // 0x10: 容量末尾
};

static_assert(std::is_standard_layout_v<VectorView<void*>>);
static_assert(offsetof(VectorView<void*>, begin) == 0x00);
static_assert(offsetof(VectorView<void*>, end) == 0x08);
static_assert(offsetof(VectorView<void*>, capacity) == 0x10);
static_assert(sizeof(VectorView<void*>) == 0x18);

// std::shared_ptr 视图（MSVC 布局）。FUN_18092A4F0 的构造上下文里，
// “指针字段 + 其 +0x08 控制块”成对出现，即 shared_ptr。
template <typename T>
struct SharedPtrView {
  T* pointer;          // 0x00: 对象指针
  void* control_block; // 0x08: 控制块
};

static_assert(std::is_standard_layout_v<SharedPtrView<void>>);
static_assert(offsetof(SharedPtrView<void>, pointer) == 0x00);
static_assert(offsetof(SharedPtrView<void>, control_block) == 0x08);
static_assert(sizeof(SharedPtrView<void>) == 0x10);

} // namespace ykkz000::civ6
