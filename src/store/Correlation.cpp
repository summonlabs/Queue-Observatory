#include "qobs/store/Correlation.hpp"

namespace qobs {
namespace {

std::uint64_t greatest_common_divisor(std::uint64_t a, std::uint64_t b) noexcept {
  while (b != 0u) {
    const std::uint64_t remainder = a % b;
    a = b;
    b = remainder;
  }
  return a;
}

}  // namespace

void reduce_fraction(std::uint64_t& numerator, std::uint64_t& denominator) noexcept {
  if (denominator == 0u) {
    // A ratio with no denominator is not a number. Normalising to 0/1 keeps the
    // record printable and unambiguous instead of producing a division by zero.
    numerator = 0;
    denominator = 1;
    return;
  }
  if (numerator == 0u) {
    denominator = 1;
    return;
  }
  const std::uint64_t divisor = greatest_common_divisor(numerator, denominator);
  if (divisor > 1u) {
    numerator /= divisor;
    denominator /= divisor;
  }
}

}  // namespace qobs
