#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ykkz000::civ6 {

// 引擎 GameCore::City::Instance。
struct City {
  struct Instance {
    void** vtable;                      // 0x000: 城市对象 vtable
    std::uint8_t unknown_0x008[0xa0];   // 0x008..0xA7: 未知
    // 0xA8: 城市 ID（Player::CityID 的序列号部分）—— Exports::City::GetID
    //       （*(uint*)(instance+0xA8)）、Lua::Utility::GetCityID
    //       （*(u32*)(city+0xA8)，另在 city+0xD8 取所属玩家短整型）。
    //       城市对象销毁后指针可能被回收复用，据该 ID 识别是否仍是同一座城市。
    std::int32_t city_id;
    std::uint8_t unknown_0x0ac[0x1bc];  // 0x0AC..0x267: 未知
    // 0x268: int（人口）—— Player::Stats::GetPopulation 遍历城市链累加
    //        city+0x268；Lua 的 GetPopulation 亦取该偏移。
    std::int32_t population;
  };
};

static_assert(std::is_standard_layout_v<City::Instance>);
static_assert(offsetof(City::Instance, city_id) == 0xa8);
static_assert(offsetof(City::Instance, population) == 0x268);

} // namespace ykkz000::civ6
