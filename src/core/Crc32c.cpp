#include "qobs/core/Crc32c.hpp"

#include <array>

namespace qobs {
namespace {

/// CRC-32C reflected polynomial (Castagnoli).
constexpr std::uint32_t kPolynomial = 0x82F63B78u;

struct Table {
  std::array<std::uint32_t, 256> entries{};

  constexpr Table() : entries{} {
    for (std::uint32_t index = 0; index < 256u; ++index) {
      std::uint32_t value = index;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1u) != 0u ? ((value >> 1u) ^ kPolynomial) : (value >> 1u);
      }
      entries[index] = value;
    }
  }
};

constexpr Table kTable{};

}  // namespace

std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept {
  std::uint32_t state = 0xFFFFFFFFu;
  for (const std::byte value : bytes) {
    const auto byte_value = std::to_integer<std::uint8_t>(value);
    state = kTable.entries[(state ^ byte_value) & 0xFFu] ^ (state >> 8u);
  }
  return ~state;
}

std::uint32_t crc32c(std::string_view text) noexcept {
  return crc32c(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                           text.size()));
}

void Crc32c::update(std::span<const std::byte> bytes) noexcept {
  std::uint32_t state = state_;
  for (const std::byte value : bytes) {
    const auto byte_value = std::to_integer<std::uint8_t>(value);
    state = kTable.entries[(state ^ byte_value) & 0xFFu] ^ (state >> 8u);
  }
  state_ = state;
  bytes_ += static_cast<std::uint64_t>(bytes.size());
}

void Crc32c::update(std::string_view text) noexcept {
  update(std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()), text.size()));
}

std::uint32_t Crc32c::value() const noexcept { return ~state_; }

}  // namespace qobs
