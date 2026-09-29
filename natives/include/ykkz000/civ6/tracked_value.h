#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <ykkz000/civ6/common.h>

// GameCore 引擎对象布局镜像：TrackedValue（“基础值 + 修正步”的产出累计结构）。
//
// City::Instance::CalculateYield(YieldTypes, TypeHash, bool) 以隐藏 sret 返回该结构：
//   rcx = 返回对象，rdx = city，r8 = yield，r9 = typeHash，栈 = flag。
//
// 布局证据（发布镜像逐条反编译，2026-09-29）：
//   * FUN_18012fc10（TrackedValue::AddStep，this = 修正子对象）把一条修正并入
//     this+0x10；本结构修正子对象位于 +0x30，故写入落在 +0x40。
//   * +0x14 为非零才走 Localization/明细路径，是子对象的“有效”标志。
//   * 基础累计 +0x10 与修正累计 +0x40 单位均为 FixedPoint<8>：1 个百分点 == 0x100，
//     +100% == 25600；修正子对象初值 0xFFFF9C00 == -25600（-100 个百分点）。
//   * 读取路径（extra::CityExtra）按 (percent × population) >> 8 折算成同一单位后
//     直接累加到 +0x40。
//
// 注：明细向量的元素类型未定，这里只按 MSVC std::vector 的三指针视图记录。
namespace ykkz000::civ6 {

struct TrackedValue {
  std::uint8_t unknown_0x00[0x10];  // 0x00..0x0F: 未知
  // 0x10: 基础累计（FixedPoint<8>）。
  std::int32_t accumulated;
  std::uint8_t valid;               // 0x14: 有效标志（AddStep 依赖其为非零）
  std::uint8_t unknown_0x15[3];     // 0x15..0x17: 未知
  VectorView<void*> steps;          // 0x18: 明细向量（+0x18 begin / +0x20 end / +0x28 cap）
  std::uint8_t unknown_0x30[0x10];  // 0x30..0x3F: 修正子对象头（+0x44 有效标志为 +0x14）
  // 0x40: 修正累计（FixedPoint<8>；25600 == +100%）。读取路径的注入目标。
  std::int32_t modifier_accumulated;
  std::uint8_t modifier_valid;             // 0x44: 修正子对象有效标志
  std::uint8_t unknown_0x45[3];            // 0x45..0x47: 未知
  VectorView<void*> modifier_steps;        // 0x48: 修正明细向量
};

static_assert(std::is_standard_layout_v<TrackedValue>);
static_assert(offsetof(TrackedValue, accumulated) == 0x10);
static_assert(offsetof(TrackedValue, valid) == 0x14);
static_assert(offsetof(TrackedValue, steps) == 0x18);
static_assert(offsetof(TrackedValue, modifier_accumulated) == 0x40);
static_assert(offsetof(TrackedValue, modifier_valid) == 0x44);
static_assert(offsetof(TrackedValue, modifier_steps) == 0x48);
static_assert(sizeof(TrackedValue) == 0x60);

} // namespace ykkz000::civ6
