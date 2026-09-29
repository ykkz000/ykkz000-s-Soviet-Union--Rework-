#pragma once

#include <cstddef>
#include <cstdint>

#include <ykkz000/bridge.h>
#include <ykkz000/civ6/city.h>
#include <ykkz000/civ6/game_manager.h>
#include <ykkz000/civ6/player.h>
#include <ykkz000/civ6/unit.h>

// 插件侧引擎访问层：把宿主服务（受校验读写、玩家/城市解析）包装成类型安全的访问器。
// Loader 不再认识任何具体行为，所有引擎读取都必须经过 host->readField 之类校验。
namespace ykkz000::plugin {

// 运行时防御上限：条目数骤增或人口/玩家索引离谱通常是 self/city 指针无效的症状，
// 此时宁可跳过写入也不要把垃圾地址写坏。
inline constexpr int kMaxEffectEntries = 64;
inline constexpr int kMaxPlausiblePopulation = 100000;
inline constexpr int kMaxPlausiblePlayerIndex = 255;
inline constexpr int kMaxPlausibleSuzerainCount = 128;

// 插件全局上下文：宿主服务与引擎入口（GetPlugin 时设置）。
struct PluginContext {
  const bridge::Host* host = nullptr;
  const bridge::EngineApi* engine = nullptr;
};

void SetContext(const bridge::Host* host);
[[nodiscard]] const PluginContext& Context();

void Log(int level, const char* message);
void LogF(int level, const char* format, ...);

// —— 成员访问层：成员引用 → 偏移，读取前一律经 host->readField 校验 ——

// 由成员指针取字段偏移。以对齐的静态哑对象为基准取成员地址，避免对空指针取址。
template <class TObj, class TField>
[[nodiscard]] std::size_t MemberOffset(TField TObj::* member) {
  static const TObj kDummy{};
  const auto base = reinterpret_cast<std::uintptr_t>(&kDummy);
  const auto field = reinterpret_cast<std::uintptr_t>(&(kDummy.*member));
  return static_cast<std::size_t>(field - base);
}

template <class TObj, class TField>
[[nodiscard]] bool TryRead(const void* base, TField TObj::* member, TField& out) {
  const bridge::Host* host = Context().host;
  if (host == nullptr || host->readField == nullptr) {
    return false;
  }
  return host->readField(base, MemberOffset(member), sizeof(TField), &out) != 0;
}

template <class TObj, class TField>
[[nodiscard]] TField TryReadOr(const void* base, TField TObj::* member, TField fallback) {
  TField value = fallback;
  (void)TryRead(base, member, value);
  return value;
}

template <class TObj, class TField>
bool TryWrite(void* base, TField TObj::* member, const TField& value) {
  const bridge::Host* host = Context().host;
  if (host == nullptr || host->writeField == nullptr) {
    return false;
  }
  return host->writeField(base, MemberOffset(member), sizeof(TField), &value) != 0;
}

// 无成员类型时的裸偏移读写。
template <typename T>
[[nodiscard]] bool TryReadAt(const void* base, std::size_t offset, T& out) {
  const bridge::Host* host = Context().host;
  if (host == nullptr || host->readField == nullptr) {
    return false;
  }
  return host->readField(base, offset, sizeof(T), &out) != 0;
}

template <typename T>
bool TryWriteAt(void* base, std::size_t offset, const T& value) {
  const bridge::Host* host = Context().host;
  if (host == nullptr || host->writeField == nullptr) {
    return false;
  }
  return host->writeField(base, offset, sizeof(T), &value) != 0;
}

[[nodiscard]] bool IsReadable(const void* address, std::size_t bytes);
[[nodiscard]] bool IsCandidateObject(const void* pointer);

// —— 玩家访问 ——
// 引擎不在玩家上存“宗主数”：宗主关系存于每个城邦的 Player::Influence::suzerain
// (+0x418)，遍历玩家向量统计 Influence::suzerain == 目标玩家类型者即为宗主数。
[[nodiscard]] bool GetPlayerVector(civ6::Player::Instance**& begin,
                                   civ6::Player::Instance**& end);
// 候选是否为玩家向量成员（精确匹配，杜绝“可读即通过”的假阳性）。
[[nodiscard]] bool IsRealPlayer(const void* candidate);
// 在玩家向量内按 +0xD8 的玩家类型精确查找（下标不保证等于玩家类型）。
[[nodiscard]] void* PlayerById(int player_id);
// 按索引直接取玩家（边界严格限制在玩家向量内）。
[[nodiscard]] void* PlayerAtIndex(int index);
// 把任意“携带玩家类型字段”的对象解析为真玩家；via 回传命中方式供诊断。
[[nodiscard]] void* ResolvePlayerFromObject(const void* object, const char*& via);
// 统计一名玩家所宗主的城邦数；任一环节不可信返回 -1。
[[nodiscard]] int CountSuzerainsOfPlayer(void* player);

} // namespace ykkz000::plugin
