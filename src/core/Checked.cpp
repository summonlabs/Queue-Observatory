#include "qobs/core/Checked.hpp"

#include <limits>

namespace qobs {
namespace checked {

std::optional<std::uint64_t> add_u64(std::uint64_t a, std::uint64_t b) noexcept {
  if (a > std::numeric_limits<std::uint64_t>::max() - b) {
    return std::nullopt;
  }
  return a + b;
}

std::optional<std::uint64_t> sub_u64(std::uint64_t a, std::uint64_t b) noexcept {
  if (b > a) {
    return std::nullopt;
  }
  return a - b;
}

std::optional<std::uint64_t> mul_u64(std::uint64_t a, std::uint64_t b) noexcept {
  if (a == 0 || b == 0) {
    return std::uint64_t{0};
  }
  if (a > std::numeric_limits<std::uint64_t>::max() / b) {
    return std::nullopt;
  }
  return a * b;
}

std::optional<std::uint64_t> div_ceil_u64(std::uint64_t a, std::uint64_t b) noexcept {
  if (b == 0) {
    return std::nullopt;
  }
  const std::uint64_t quotient = a / b;
  const std::uint64_t remainder = a % b;
  if (remainder == 0) {
    return quotient;
  }
  return add_u64(quotient, 1);
}

std::optional<std::int64_t> add_i64(std::int64_t a, std::int64_t b) noexcept {
  if (b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) {
    return std::nullopt;
  }
  if (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b) {
    return std::nullopt;
  }
  return a + b;
}

std::optional<std::int64_t> sub_i64(std::int64_t a, std::int64_t b) noexcept {
  if (b == std::numeric_limits<std::int64_t>::min()) {
    return a >= 0 ? std::nullopt : add_i64(a + std::numeric_limits<std::int64_t>::max(), 1);
  }
  return add_i64(a, -b);
}

std::optional<std::int64_t> mul_i64(std::int64_t a, std::int64_t b) noexcept {
  if (a == 0 || b == 0) {
    return std::int64_t{0};
  }
  if (a == -1 && b == std::numeric_limits<std::int64_t>::min()) {
    return std::nullopt;
  }
  if (b == -1 && a == std::numeric_limits<std::int64_t>::min()) {
    return std::nullopt;
  }
  const std::int64_t result = a * b;
  if (result / b != a) {
    return std::nullopt;
  }
  return result;
}

bool fits_size_t(std::uint64_t value) noexcept {
  return value <= static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max());
}

std::optional<std::size_t> to_size(std::uint64_t value) noexcept {
  if (!fits_size_t(value)) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(value);
}

Result<std::uint64_t> checked_add(std::uint64_t a, std::uint64_t b, const char* context) {
  const auto sum = add_u64(a, b);
  if (!sum.has_value()) {
    return Result<std::uint64_t>::failure(ErrorCode::Overflow,
                                          std::string(context) + ": addition overflow");
  }
  return *sum;
}

Result<std::uint64_t> checked_mul(std::uint64_t a, std::uint64_t b, const char* context) {
  const auto product = mul_u64(a, b);
  if (!product.has_value()) {
    return Result<std::uint64_t>::failure(ErrorCode::Overflow,
                                          std::string(context) + ": multiplication overflow");
  }
  return *product;
}

Result<std::size_t> checked_size(std::uint64_t value, const char* context) {
  const auto converted = to_size(value);
  if (!converted.has_value()) {
    return Result<std::size_t>::failure(
        ErrorCode::OutOfRange, std::string(context) + ": value does not fit in size_t");
  }
  return *converted;
}

}  // namespace checked
}  // namespace qobs
