#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qobs {

/// Small, dependency-free text utilities. All of them are deterministic; none
/// of them consult the locale.
namespace text {

[[nodiscard]] bool starts_with(std::string_view value, std::string_view prefix) noexcept;
[[nodiscard]] bool ends_with(std::string_view value, std::string_view suffix) noexcept;
[[nodiscard]] std::string_view trim(std::string_view value) noexcept;
[[nodiscard]] std::string to_lower_ascii(std::string_view value);
[[nodiscard]] std::string to_upper_ascii(std::string_view value);

/// Split on a single character. Empty fields are preserved so that malformed
/// input cannot silently lose columns.
[[nodiscard]] std::vector<std::string_view> split(std::string_view value, char separator);

[[nodiscard]] std::string join(const std::vector<std::string>& parts, std::string_view separator);

/// Render bytes as lowercase hexadecimal.
[[nodiscard]] std::string to_hex(std::span<const std::byte> bytes);
[[nodiscard]] std::string to_hex(std::uint64_t value, std::size_t min_digits);

/// Escape a string so that it can be embedded in a JSON document. Also makes
/// control characters visible, which keeps CLI output single-line and safe.
[[nodiscard]] std::string escape_json(std::string_view value);

/// Replace control characters with a dot and truncate to the given limit,
/// appending an ellipsis marker when truncation happened. Used for anything
/// echoed back to a terminal.
[[nodiscard]] std::string sanitize_for_display(std::string_view value, std::size_t limit);

[[nodiscard]] std::string pad_right(std::string value, std::size_t width);
[[nodiscard]] std::string pad_left(std::string value, std::size_t width);

/// Parse an unsigned integer with strict syntax: no sign, no whitespace, no
/// trailing characters, and overflow reported.
[[nodiscard]] bool parse_u64(std::string_view value, std::uint64_t& out) noexcept;
[[nodiscard]] bool parse_i64(std::string_view value, std::int64_t& out) noexcept;

/// Parse a hexadecimal integer with an optional 0x prefix.
[[nodiscard]] bool parse_hex_u64(std::string_view value, std::uint64_t& out) noexcept;

/// Deterministic FNV-1a 64-bit hash. Used for stable ordering tie-breaks and
/// sampling decisions where reproducibility matters more than collision
/// resistance. This is not a cryptographic hash.
[[nodiscard]] std::uint64_t fnv1a64(std::string_view value) noexcept;

}  // namespace text
}  // namespace qobs
