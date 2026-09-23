#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "qobs/core/Result.hpp"

namespace qobs {

/// A minimal, bounded, dependency-free JSON document model.
///
/// Queue Observatory accepts observation batches as JSON. Because that input is
/// external, the parser enforces explicit limits on depth, node count, string
/// length, member count and document size, and it reports structural errors
/// instead of repairing them.
///
/// Object members are stored as parallel key and value vectors, which keeps the
/// type definition non-recursive and keeps insertion order (and therefore
/// serialized output order) exactly as written.
class JsonValue {
 public:
  enum class Type : std::uint8_t { Null, Bool, Int, Uint, Double, String, Array, Object };

  JsonValue() = default;

  [[nodiscard]] static JsonValue make_null();
  [[nodiscard]] static JsonValue make_bool(bool value);
  [[nodiscard]] static JsonValue make_int(std::int64_t value);
  [[nodiscard]] static JsonValue make_uint(std::uint64_t value);
  [[nodiscard]] static JsonValue make_double(double value);
  [[nodiscard]] static JsonValue make_string(std::string value);
  [[nodiscard]] static JsonValue make_array();
  [[nodiscard]] static JsonValue make_object();

  [[nodiscard]] Type type() const noexcept { return type_; }
  [[nodiscard]] bool is_null() const noexcept { return type_ == Type::Null; }
  [[nodiscard]] bool is_bool() const noexcept { return type_ == Type::Bool; }
  [[nodiscard]] bool is_int() const noexcept { return type_ == Type::Int; }
  [[nodiscard]] bool is_uint() const noexcept { return type_ == Type::Uint; }
  [[nodiscard]] bool is_double() const noexcept { return type_ == Type::Double; }
  [[nodiscard]] bool is_string() const noexcept { return type_ == Type::String; }
  [[nodiscard]] bool is_array() const noexcept { return type_ == Type::Array; }
  [[nodiscard]] bool is_object() const noexcept { return type_ == Type::Object; }
  [[nodiscard]] bool is_number() const noexcept {
    return type_ == Type::Int || type_ == Type::Uint || type_ == Type::Double;
  }

  [[nodiscard]] bool as_bool() const noexcept { return bool_value_; }
  [[nodiscard]] std::int64_t as_int() const noexcept { return int_value_; }
  [[nodiscard]] std::uint64_t as_uint() const noexcept { return uint_value_; }
  [[nodiscard]] double as_double() const noexcept { return double_value_; }
  [[nodiscard]] const std::string& as_string() const noexcept { return string_value_; }

  [[nodiscard]] const std::vector<JsonValue>& items() const noexcept { return children_; }
  [[nodiscard]] const std::vector<std::string>& keys() const noexcept { return keys_; }

  /// Looks up a member. Returns nullptr when absent. Keys are compared exactly;
  /// duplicate keys are a parse error, so a lookup is unambiguous.
  [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept;

  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] std::string_view key_at(std::size_t index) const noexcept;
  [[nodiscard]] const JsonValue& value_at(std::size_t index) const noexcept;

  /// Appends an element to an array value.
  void push(JsonValue value);

  /// Appends a member to an object value.
  void set(std::string key, JsonValue value);

  [[nodiscard]] std::string_view type_name() const noexcept;

 private:
  Type type_{Type::Null};
  bool bool_value_{false};
  std::int64_t int_value_{0};
  std::uint64_t uint_value_{0};
  double double_value_{0.0};
  std::string string_value_{};
  std::vector<std::string> keys_{};
  std::vector<JsonValue> children_{};
};

/// Structural limits applied while parsing.
struct JsonLimits {
  std::size_t max_document_bytes{8u * 1024u * 1024u};
  std::size_t max_depth{32};
  std::size_t max_nodes{200000};
  std::size_t max_string_bytes{4096};
  std::size_t max_object_members{256};
  std::size_t max_array_elements{4096};

  friend bool operator==(const JsonLimits&, const JsonLimits&) = default;
};

/// Parse a complete JSON document. Trailing content after the top-level value is
/// an error: a partially consumed stream is never silently accepted.
[[nodiscard]] Result<JsonValue> parse_json(std::string_view text, const JsonLimits& limits);

/// Parse the first JSON value in a buffer and report how many bytes it used.
/// Used for newline-delimited documents, where the caller must know exactly
/// where the value ended.
[[nodiscard]] Result<JsonValue> parse_json_prefix(std::string_view text, const JsonLimits& limits,
                                                  std::size_t& consumed);

/// Deterministic JSON writer.
///
/// Object member order is the order in which keys are written, so byte-for-byte
/// identical output is produced for identical inputs. Numbers are written from
/// integers or from an explicit fixed-point scale; no double is ever formatted
/// into runtime output, which keeps exports reproducible across platforms.
class JsonWriter {
 public:
  JsonWriter() = default;
  explicit JsonWriter(bool pretty) : pretty_(pretty) {}

  [[nodiscard]] Status begin_object();
  [[nodiscard]] Status end_object();
  [[nodiscard]] Status begin_array();
  [[nodiscard]] Status end_array();
  [[nodiscard]] Status key(std::string_view name);
  [[nodiscard]] Status value_string(std::string_view value);
  [[nodiscard]] Status value_u64(std::uint64_t value);
  [[nodiscard]] Status value_i64(std::int64_t value);
  [[nodiscard]] Status value_bool(bool value);
  [[nodiscard]] Status value_null();

  /// Write an exact decimal rendering of units * 10^-scale without ever using
  /// binary floating point.
  [[nodiscard]] Status value_fixed(std::uint64_t units, std::uint32_t scale);

  /// Write a value that the caller has already validated as a JSON fragment.
  [[nodiscard]] Status value_passthrough(std::string_view fragment);

  [[nodiscard]] Status member_string(std::string_view name, std::string_view value);
  [[nodiscard]] Status member_u64(std::string_view name, std::uint64_t value);
  [[nodiscard]] Status member_i64(std::string_view name, std::int64_t value);
  [[nodiscard]] Status member_bool(std::string_view name, bool value);

  [[nodiscard]] bool ok() const noexcept { return error_.code() == ErrorCode::Ok; }
  [[nodiscard]] const Error& error() const noexcept { return error_; }

  /// The document produced so far. Only meaningful once ok() is true.
  [[nodiscard]] const std::string& buffer() const noexcept { return buffer_; }
  [[nodiscard]] std::string take() { return std::move(buffer_); }

 private:
  enum class Frame : std::uint8_t { Array, Object };

  [[nodiscard]] Status before_value();
  [[nodiscard]] Status ready() const;
  [[nodiscard]] Status fail(ErrorCode code, std::string message);
  void newline_indent();

  std::string buffer_{};
  std::vector<Frame> stack_{};
  std::vector<bool> has_items_{};
  /// True between writing a member name and writing its value. Only meaningful
  /// while the innermost open frame is an object.
  bool awaiting_value_{false};
  bool pretty_{false};
  Error error_{};
};

}  // namespace qobs
