#include "qobs/core/Text.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace qobs {
namespace text {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

constexpr bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

constexpr int digit_value(char c) noexcept { return static_cast<int>(c) - static_cast<int>('0'); }

}  // namespace

bool starts_with(std::string_view value, std::string_view prefix) noexcept {
  return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(std::string_view value, std::string_view suffix) noexcept {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string_view trim(std::string_view value) noexcept {
  std::size_t begin = 0;
  while (begin < value.size()) {
    const char c = value[begin];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      ++begin;
    } else {
      break;
    }
  }
  std::size_t end = value.size();
  while (end > begin) {
    const char c = value[end - 1];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      --end;
    } else {
      break;
    }
  }
  return value.substr(begin, end - begin);
}

std::string to_lower_ascii(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    if (c >= 'A' && c <= 'Z') {
      out.push_back(static_cast<char>(c - 'A' + 'a'));
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string to_upper_ascii(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    if (c >= 'a' && c <= 'z') {
      out.push_back(static_cast<char>(c - 'a' + 'A'));
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::vector<std::string_view> split(std::string_view value, char separator) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  while (true) {
    const std::size_t position = value.find(separator, start);
    if (position == std::string_view::npos) {
      parts.push_back(value.substr(start));
      break;
    }
    parts.push_back(value.substr(start, position - start));
    start = position + 1;
  }
  return parts;
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
  std::string out;
  for (std::size_t index = 0; index < parts.size(); ++index) {
    if (index != 0) {
      out.append(separator);
    }
    out.append(parts[index]);
  }
  return out;
}

std::string to_hex(std::span<const std::byte> bytes) {
  std::string out;
  out.reserve(bytes.size() * 2u);
  for (const std::byte value : bytes) {
    const auto byte_value = static_cast<unsigned int>(std::to_integer<unsigned char>(value));
    out.push_back(kHexDigits[(byte_value >> 4u) & 0x0Fu]);
    out.push_back(kHexDigits[byte_value & 0x0Fu]);
  }
  return out;
}

std::string to_hex(std::uint64_t value, std::size_t min_digits) {
  std::string digits;
  std::uint64_t remaining = value;
  do {
    digits.push_back(kHexDigits[remaining & 0x0Fu]);
    remaining >>= 4u;
  } while (remaining != 0);
  while (digits.size() < min_digits) {
    digits.push_back('0');
  }
  std::reverse(digits.begin(), digits.end());
  return digits;
}

std::string escape_json(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 8u);
  for (const char c : value) {
    const auto byte_value = static_cast<unsigned char>(c);
    switch (c) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\b':
        out.append("\\b");
        break;
      case '\f':
        out.append("\\f");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        if (byte_value < 0x20u) {
          out.append("\\u00");
          out.push_back(kHexDigits[(byte_value >> 4u) & 0x0Fu]);
          out.push_back(kHexDigits[byte_value & 0x0Fu]);
        } else {
          out.push_back(c);
        }
        break;
    }
  }
  return out;
}

std::string sanitize_for_display(std::string_view value, std::size_t limit) {
  std::string out;
  const std::size_t take = std::min(value.size(), limit);
  out.reserve(take + 3u);
  for (std::size_t index = 0; index < take; ++index) {
    const auto byte_value = static_cast<unsigned char>(value[index]);
    out.push_back(byte_value < 0x20u || byte_value == 0x7Fu ? '.' : value[index]);
  }
  if (value.size() > limit) {
    out.append("...");
  }
  return out;
}

std::string pad_right(std::string value, std::size_t width) {
  if (value.size() < width) {
    value.append(width - value.size(), ' ');
  }
  return value;
}

std::string pad_left(std::string value, std::size_t width) {
  if (value.size() < width) {
    value.insert(0, width - value.size(), ' ');
  }
  return value;
}

bool parse_u64(std::string_view value, std::uint64_t& out) noexcept {
  if (value.empty()) {
    return false;
  }
  std::uint64_t accumulator = 0;
  for (const char c : value) {
    if (!is_digit(c)) {
      return false;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(digit_value(c));
    if (accumulator > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) {
      return false;
    }
    accumulator = accumulator * 10u + digit;
  }
  out = accumulator;
  return true;
}

bool parse_i64(std::string_view value, std::int64_t& out) noexcept {
  if (value.empty()) {
    return false;
  }
  bool negative = false;
  std::string_view digits = value;
  if (value.front() == '-') {
    negative = true;
    digits = value.substr(1);
  } else if (value.front() == '+') {
    return false;
  }
  if (digits.empty()) {
    return false;
  }
  std::uint64_t magnitude = 0;
  if (!parse_u64(digits, magnitude)) {
    return false;
  }
  const auto positive_limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (negative) {
    if (magnitude > positive_limit + 1u) {
      return false;
    }
    if (magnitude == positive_limit + 1u) {
      out = std::numeric_limits<std::int64_t>::min();
      return true;
    }
    out = -static_cast<std::int64_t>(magnitude);
    return true;
  }
  if (magnitude > positive_limit) {
    return false;
  }
  out = static_cast<std::int64_t>(magnitude);
  return true;
}

bool parse_hex_u64(std::string_view value, std::uint64_t& out) noexcept {
  std::string_view digits = value;
  if (starts_with(digits, "0x") || starts_with(digits, "0X")) {
    digits = digits.substr(2);
  }
  if (digits.empty() || digits.size() > 16u) {
    return false;
  }
  std::uint64_t accumulator = 0;
  for (const char c : digits) {
    std::uint64_t digit = 0;
    if (c >= '0' && c <= '9') {
      digit = static_cast<std::uint64_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<std::uint64_t>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      digit = static_cast<std::uint64_t>(c - 'A' + 10);
    } else {
      return false;
    }
    accumulator = (accumulator << 4u) | digit;
  }
  out = accumulator;
  return true;
}

std::uint64_t fnv1a64(std::string_view value) noexcept {
  std::uint64_t hash = 14695981039346656037ull;
  for (const char c : value) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
    hash *= 1099511628211ull;
  }
  return hash;
}

}  // namespace text
}  // namespace qobs
