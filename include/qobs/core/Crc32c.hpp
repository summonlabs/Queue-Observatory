#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace qobs {

/// CRC-32C (Castagnoli), software implementation. Used purely as an integrity
/// check for persisted and transported bytes; it is not a cryptographic
/// guarantee and is never described as one.
[[nodiscard]] std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept;
[[nodiscard]] std::uint32_t crc32c(std::string_view text) noexcept;

/// Incremental CRC-32C.
class Crc32c {
 public:
  Crc32c() = default;

  void update(std::span<const std::byte> bytes) noexcept;
  void update(std::string_view text) noexcept;

  /// Current value without disturbing the running state.
  [[nodiscard]] std::uint32_t value() const noexcept;
  [[nodiscard]] std::uint64_t bytes_processed() const noexcept { return bytes_; }

 private:
  std::uint32_t state_{0xFFFFFFFFu};
  std::uint64_t bytes_{0};
};

}  // namespace qobs
