#pragma once

#include <cstdint>
#include <string_view>

namespace ykkz000::bridge {

[[nodiscard]] constexpr std::uint32_t crc32_byte(std::uint32_t crc, std::uint8_t byte) noexcept {
  crc ^= byte;
  for (int bit = 0; bit < 8; ++bit) {
    crc = (crc >> 1u) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc;
}

// 与引擎 GameCore::Utilities::MakeHash 一致：
// CRC-32，多项式 0xEDB88320，初值 0xFFFFFFFF，无最终取反。
[[nodiscard]] constexpr std::uint32_t crc32(std::string_view text) noexcept {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const char ch : text) {
    crc = crc32_byte(crc, static_cast<std::uint8_t>(ch));
  }
  return crc;
}

[[nodiscard]] inline std::uint32_t makeHash(const char* text) noexcept {
  return text ? crc32(std::string_view{text}) : 0u;
}

} // namespace ykkz000::bridge
