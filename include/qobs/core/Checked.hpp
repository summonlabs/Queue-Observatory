#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

#include "qobs/core/Result.hpp"

namespace qobs {

/// Checked arithmetic for every quantity that originates outside this runtime:
/// wire payloads, persistence headers, configuration, and query parameters.
/// Overflow is a reported error, never a wrapped value.
namespace checked {

[[nodiscard]] std::optional<std::uint64_t> add_u64(std::uint64_t a, std::uint64_t b) noexcept;
[[nodiscard]] std::optional<std::uint64_t> sub_u64(std::uint64_t a, std::uint64_t b) noexcept;
[[nodiscard]] std::optional<std::uint64_t> mul_u64(std::uint64_t a, std::uint64_t b) noexcept;
[[nodiscard]] std::optional<std::uint64_t> div_ceil_u64(std::uint64_t a, std::uint64_t b) noexcept;

[[nodiscard]] std::optional<std::int64_t> add_i64(std::int64_t a, std::int64_t b) noexcept;
[[nodiscard]] std::optional<std::int64_t> sub_i64(std::int64_t a, std::int64_t b) noexcept;
[[nodiscard]] std::optional<std::int64_t> mul_i64(std::int64_t a, std::int64_t b) noexcept;

/// True when the value fits in std::size_t on this platform.
[[nodiscard]] bool fits_size_t(std::uint64_t value) noexcept;

/// Convert a wire-supplied size to std::size_t, or nothing when it cannot fit.
[[nodiscard]] std::optional<std::size_t> to_size(std::uint64_t value) noexcept;

/// Narrow an unsigned quantity to a smaller unsigned type when it fits.
template <class T>
[[nodiscard]] std::optional<T> narrow_u(std::uint64_t value) noexcept {
  static_assert(std::is_unsigned_v<T>, "narrow_u requires an unsigned target");
  if (value > static_cast<std::uint64_t>(std::numeric_limits<T>::max())) {
    return std::nullopt;
  }
  return static_cast<T>(value);
}

/// Result-returning variants used at API boundaries.
[[nodiscard]] Result<std::uint64_t> checked_add(std::uint64_t a, std::uint64_t b,
                                                const char* context);
[[nodiscard]] Result<std::uint64_t> checked_mul(std::uint64_t a, std::uint64_t b,
                                                const char* context);
[[nodiscard]] Result<std::size_t> checked_size(std::uint64_t value, const char* context);

/// Clamp helper, used only where a bounded, conservative saturation is the
/// documented policy (never for externally derived buffer sizes).
template <class T>
[[nodiscard]] constexpr T clamp(T value, T low, T high) noexcept {
  return value < low ? low : (value > high ? high : value);
}

}  // namespace checked
}  // namespace qobs
