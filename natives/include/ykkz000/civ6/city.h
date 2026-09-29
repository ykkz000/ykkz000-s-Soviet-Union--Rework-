#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ykkz000::civ6 {

// 引擎的城市产出缓存条目（`City::Instance::yield_cache` 指向的数据，每条目 8 字节，
// 按 YieldType 索引）。
//
// 证据（发布镜像 FUN_1801332F0，即 CalculateYield 的调用者）：
//   entry = *(longlong*)(city + 0x1950) + yield * 8;
//   if (*(char*)(entry + 4) == 0) { CalculateYield(...); *entry = combine(...);
//                                   *(char*)(entry + 4) = 1; }  // 重算并置有效
//   *out = *entry;                                              // 命中即用旧值
// 即 valid == 0 时才重算，valid != 0 时直接返回缓存值。本模组的侧表（叠加数据）变化
// 不会清这个标志，故 Apply/Remove 必须主动清 valid 迫使重算。
struct YieldCacheEntry {
  std::int32_t value;      // 0x00: 缓存值（引擎重算时写回）
  std::uint8_t valid;      // 0x04: 有效标志（0 = 下次读取需重算）
  std::uint8_t pad_0x05[3];
};

static_assert(sizeof(YieldCacheEntry) == 8);

// 引擎 GameCore::City::Instance。
//
// 仅描述布局，不含任何本模组叠加数据（叠加数据见 extra/）。每个已知字段以
// static_assert 锁定偏移；注释给出“偏移 + 含义 + 证据函数”。
struct City {
  struct Instance {
    void** vtable;                      // 0x000: 城市对象 vtable
    std::uint8_t unknown_0x008[0xa0];   // 0x008..0xA7: 未知
    // 0xA8: 城市 ID（Player::CityID 的序列号部分）—— Exports::City::GetID
    //       （*(uint*)(instance+0xA8)）、Lua::Utility::GetCityID
    //       （*(u32*)(city+0xA8)，另在 city+0xD8 取所属玩家短整型）。
    //       城市对象销毁后指针可能被回收复用，据该 ID 识别是否仍是同一座城市。
    //       City::Instance::ChangePopulation/ChangeYieldModifier 组装通知时取该处。
    std::int32_t city_id;
    std::uint8_t unknown_0x0ac[0x2c];   // 0x0AC..0x0D7: 未知
    // 0xD8: PlayerTypes —— 城市所属玩家。City::Instance::CalculateYield 以
    //       *(u32*)(city+0xD8) 取玩家（如喂给 Context::Globals::GetPlayer 与
    //       Player::Governors::Get）；ChangePopulation/ChangeYieldModifier 组装
    //       CityID 时取该处低 16 位；序列化时亦取此处。
    std::int32_t owner;
    std::uint8_t unknown_0x0dc[0x18c];  // 0x0DC..0x267: 未知
    // 0x268: int（人口）—— Player::Stats::GetPopulation 遍历城市链累加
    //        city+0x268；Lua 的 GetPopulation 亦取该偏移；ChangePopulation 以
    //        `0 < *(int*)(city+0x268)+delta` 作保护判断。
    std::int32_t population;
    std::uint8_t unknown_0x26c[0x234];  // 0x26C..0x49F: 未知

    // —— game effects 城市产出修正（0x4B0 起）——
    // 证据：FUN_180131cf0（City::Instance::ChangeYieldModifier）取
    //   v = FUN_18072a920(city+0x4A0)；反汇编确认 FUN_18072a920 返回 param_1+0x10，
    //   故 v = city+0x4B0：v[0](+0x00)=int32 数组指针、v[2](+0x10)=计数、v+0x18=越界
    //   回落标量；`yield < count ? ptr[yield] += amount : fallback += amount`。
    // 注：此前文档把 +0x4B0 直接写作“数组起点”，本轮修正为下述偏移。
    std::uint8_t unknown_0x4a0[0x10];   // 0x4A0..0x4AF: 修正容器头（+0x08 为树根指针）
    std::int32_t* yield_modifier_game_effects_data;    // 0x4B0: 修正数组（按 YieldTypes）
    std::uint8_t unknown_0x4b8[8];                     // 0x4B8..0x4BF: 未知
    std::int32_t yield_modifier_game_effects_count;    // 0x4C0: 计数（按 (int)qword 读）
    std::uint8_t unknown_0x4c4[4];                     // 0x4C4..0x4C7: 槽内高 32 位
    std::int32_t yield_modifier_game_effects_fallback; // 0x4C8: 越界/无匹配时的回落累加
    std::uint8_t unknown_0x4cc[0x44];   // 0x4CC..0x50F: 未知

    // —— 总督头衔产出修正（0x510 起）——
    // 证据：CalculateYield（0x12FF20）0x180130A50 块 × Governor::GetNumTitles；
    //       工具提示字符串 LOC_CITY_YIELD_MODIFIER_PER_GOVERNOR_TITLE_TOOLTIP
    //       （0x1809CAC68）在该函数 0x180130AFB/0x180130B19 处被引用。
    //       偏移按同构布局记录，本轮未逐条反汇编验证。
    std::int32_t* yield_modifier_per_governor_title_data;    // 0x510
    std::uint8_t unknown_0x518[8];                           // 0x518..0x51F: 未知
    std::int32_t yield_modifier_per_governor_title_count;    // 0x520
    std::uint8_t unknown_0x524[4];                           // 0x524..0x527
    std::int32_t yield_modifier_per_governor_title_fallback; // 0x528
    std::uint8_t unknown_0x52c[0x14];   // 0x52C..0x53F: 未知

    // —— 平铺产出数组（0x540 起，语义未验证）——
    // 证据：CalculateYield 0x1801306C7 块把该数组 ×0x100 后加进基础累计 +0x10。
    std::int32_t* flat_yields_0x540_data;    // 0x540
    std::uint8_t unknown_0x548[8];           // 0x548..0x54F: 未知
    std::int32_t flat_yields_0x540_count;    // 0x550
    std::uint8_t unknown_0x554[4];           // 0x554..0x557
    std::int32_t flat_yields_0x540_fallback; // 0x558
    std::uint8_t unknown_0x55c[0x134];  // 0x55C..0x68F: 未知

    // —— 每人口产出数组（0x690 起，单位 FixedPoint<8>）——
    // 证据：City::Instance::GetYieldFromPopulation（0x133780，在 CalculateYield 的
    //       0x180130013 处被调用）：iVar5×count×0x100 累加。
    //       偏移按同构布局记录，本轮未逐条反汇编验证。
    std::int32_t* per_population_yields_data;    // 0x690
    std::uint8_t unknown_0x698[8];               // 0x698..0x69F: 未知
    std::int32_t per_population_yields_count;    // 0x6A0
    std::uint8_t unknown_0x6a4[4];               // 0x6A4..0x6A7
    std::int32_t per_population_yields_fallback; // 0x6A8
    std::uint8_t unknown_0x6ac[4];               // 0x6AC..0x6AF: 尾部对齐

    // —— 城市产出缓存（0x1950）——
    // 证据（发布镜像 FUN_1801332F0）：entry = *(longlong*)(city+0x1950) + yield*8；
    //   valid 在 entry+4，valid == 0 时引擎调用 CalculateYield 重算并写回 value（+0）
    //   后置 1，否则直接返回缓存值。侧表变化需清 valid（见 YieldCacheEntry 注释）。
    //   FUN_180951550()+0x2A0 == 0 时引擎为“无缓存”模式（总是重算），此时失效无害。
    std::uint8_t unknown_0x6b0[0x1950 - 0x6b0]; // 0x6B0..0x194F: 未知
    void* yield_cache;                          // 0x1950: YieldCacheEntry 向量数据
  };
};

static_assert(std::is_standard_layout_v<City::Instance>);
static_assert(offsetof(City::Instance, city_id) == 0xa8);
static_assert(offsetof(City::Instance, owner) == 0xd8);
static_assert(offsetof(City::Instance, population) == 0x268);
static_assert(offsetof(City::Instance, yield_modifier_game_effects_data) == 0x4b0);
static_assert(offsetof(City::Instance, yield_modifier_game_effects_count) == 0x4c0);
static_assert(offsetof(City::Instance, yield_modifier_game_effects_fallback) == 0x4c8);
static_assert(offsetof(City::Instance, yield_modifier_per_governor_title_data) == 0x510);
static_assert(offsetof(City::Instance, yield_modifier_per_governor_title_count) == 0x520);
static_assert(offsetof(City::Instance, yield_modifier_per_governor_title_fallback) == 0x528);
static_assert(offsetof(City::Instance, flat_yields_0x540_data) == 0x540);
static_assert(offsetof(City::Instance, flat_yields_0x540_count) == 0x550);
static_assert(offsetof(City::Instance, flat_yields_0x540_fallback) == 0x558);
static_assert(offsetof(City::Instance, per_population_yields_data) == 0x690);
static_assert(offsetof(City::Instance, per_population_yields_count) == 0x6a0);
static_assert(offsetof(City::Instance, per_population_yields_fallback) == 0x6a8);
static_assert(offsetof(City::Instance, yield_cache) == 0x1950);

} // namespace ykkz000::civ6
