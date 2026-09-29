#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ykkz000::civ6 {

// 引擎 GameCore::District::Instance（本模组只用其指针，不读业务字段）。
struct District {
  struct Instance {
    void** vtable;                     // 0x000: 区域对象 vtable
    std::uint8_t unknown_0x008[0xb8];  // 0x008..0x0BF: 未知
    // 0x0C0: PlotCoord（8 字节，两个 int32）—— District::Instance::GetOwner
    //        以 this+0xC0 查地图，再取地块所有者（Plot+0x1C 的 char）。
    std::uint8_t plot_coord[0x8];
  };
};

static_assert(std::is_standard_layout_v<District::Instance>);
static_assert(offsetof(District::Instance, plot_coord) == 0xc0);
static_assert(sizeof(District::Instance::plot_coord) == 0x8);

} // namespace ykkz000::civ6
