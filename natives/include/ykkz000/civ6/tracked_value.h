#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <ykkz000/civ6/common.h>

// GameCore 引擎对象布局镜像：TrackedValue（“基础值 + 修正步”的产出累计结构）。
//
// City::Instance::CalculateYield(YieldTypes, TypeHash, bool) 以隐藏 sret 返回该结构：
//   rcx = 返回对象，rdx = city，r8 = yield，r9 = typeHash，栈 = record_steps。
//
// 布局证据（发布镜像逐条反编译，2026-09-29）：
//   * CalculateYield（0x12FF20）把返回对象初始化为两个同构的 0x30 字节子对象：
//       +0x00 has_min=0 / +0x08 has_max=0 / +0x10 value=0 / +0x14 record_steps /
//       +0x18 明细向量；modifier 子对象（+0x30）额外把 +0x30 has_min 置 1、
//       +0x34 min 置 0xFFFF9C00（-25600 == -100% 下限）。
//   * 明细步 AddStep（0x12FC10）把一条修正并入 this+0x10，this 即“修正子对象”；
//     其 step 形参与上述子对象同构（便捷重载 0x12FCE0 以全零构造：has_min/has_max/
//     flag 置 0、只写 value）。
//   * 常数 0x100 表示 +1 个百分点（FixedPoint<8>）：+100% == 25600。
//   * 合成（发布 FUN_180076460）：final = clamp(base) + clamp(base) * clamp(modifier) / 25600。
//   * 读取路径（本模组侧表）按 (percent × population) >> 8 折算成 modifier 单位后经
//     AddStep 追加，与引擎自带的修正来源（game effects / 宗教 / 总督头衔…）完全同构。
namespace ykkz000::civ6 {

// 引擎“带上下限的定点值 + 明细步”结构（FixedPoint<8>，256 == +1%）。
//
// 取值先按上界向下夹（has_max 非零时 value = min(value, max)），再按下界向上夹
// （has_min 非零时 value = max(value, min)）；两个开关都为 0 时取 value 原值。
// 作为 AddStep 的 step 形参时，引擎只写 value 并把其余字段清零。
struct YieldValue {
  bool has_min;                 // 0x00: 非零则启用下界钳制
  std::int32_t min;             // 0x04: 下界（FixedPoint<8>）
  bool has_max;                 // 0x08: 非零则启用上界钳制
  std::int32_t max;             // 0x0c: 上界（FixedPoint<8>）
  std::int32_t value;           // 0x10: 累计/步长（FixedPoint<8>，256 == +1%）
  bool flag;                    // 0x14: 明细记录开关（作为 step 时引擎传 0）
  std::uint8_t unknown_0x15[3]; // 0x15..0x17: 未知/对齐
  VectorView<void*> steps;      // 0x18: 明细向量（begin/end/cap）
};

static_assert(std::is_standard_layout_v<YieldValue>);
static_assert(offsetof(YieldValue, has_min) == 0x00);
static_assert(offsetof(YieldValue, min) == 0x04);
static_assert(offsetof(YieldValue, has_max) == 0x08);
static_assert(offsetof(YieldValue, max) == 0x0c);
static_assert(offsetof(YieldValue, value) == 0x10);
static_assert(offsetof(YieldValue, flag) == 0x14);
static_assert(offsetof(YieldValue, steps) == 0x18);
static_assert(sizeof(YieldValue) == 0x30);

// CalculateYield 的返回对象：基础累计子对象（+0x00）与修正子对象（+0x30）。
//   * base.value 为引擎的“基础产出”，modifier.value 为百分比修正累计
//     （25600 == +100%）；modifier.min 由 CalculateYield 预置为 -25600。
struct TrackedValue {
  YieldValue base;     // 0x00: 基础累计（base.value 即基础产出）
  YieldValue modifier; // 0x30: 修正子对象（modifier.value 即修正累计，AddStep 的 this）
};

static_assert(std::is_standard_layout_v<TrackedValue>);
static_assert(offsetof(TrackedValue, base) == 0x00);
static_assert(offsetof(TrackedValue, modifier) == 0x30);
static_assert(sizeof(TrackedValue) == 0x60);

} // namespace ykkz000::civ6
