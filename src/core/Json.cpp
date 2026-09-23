#include "qobs/core/Json.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>

#include "qobs/core/Checked.hpp"
#include "qobs/core/Text.hpp"

namespace qobs {
namespace {

bool is_json_whitespace(char c) noexcept {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

void append_utf8(std::string& out, std::uint32_t code_point) {
  if (code_point < 0x80u) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point < 0x800u) {
    out.push_back(static_cast<char>(0xC0u | (code_point >> 6u)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
  } else if (code_point < 0x10000u) {
    out.push_back(static_cast<char>(0xE0u | (code_point >> 12u)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 6u) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
  } else {
    out.push_back(static_cast<char>(0xF0u | (code_point >> 18u)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 12u) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((code_point >> 6u) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
  }
}

class Parser {
 public:
  Parser(std::string_view text, const JsonLimits& limits) : text_(text), limits_(limits) {}

  Result<JsonValue> parse_document() {
    std::size_t index = 0;
    auto value = parse_value(index, 0);
    if (!value.has_value()) {
      return value.error();
    }
    skip_whitespace(index);
    if (index != text_.size()) {
      return fail(ErrorCode::ParseError, "unexpected trailing content after the JSON document");
    }
    return std::move(value).value();
  }

  Result<JsonValue> parse_prefix(std::size_t& consumed) {
    std::size_t index = 0;
    auto value = parse_value(index, 0);
    if (!value.has_value()) {
      return value.error();
    }
    skip_whitespace(index);
    consumed = index;
    return std::move(value).value();
  }

 private:
  Result<JsonValue> fail(ErrorCode code, std::string message) const {
    return Result<JsonValue>::failure(code, std::move(message));
  }

  void skip_whitespace(std::size_t& index) const noexcept {
    while (index < text_.size() && is_json_whitespace(text_[index])) {
      ++index;
    }
  }

  bool at_end(std::size_t index) const noexcept { return index >= text_.size(); }

  Result<JsonValue> parse_value(std::size_t& index, std::size_t depth) {
    if (depth > limits_.max_depth) {
      return fail(ErrorCode::LimitExceeded, "JSON nesting depth exceeds the configured limit");
    }
    ++nodes_;
    if (nodes_ > limits_.max_nodes) {
      return fail(ErrorCode::LimitExceeded, "JSON node count exceeds the configured limit");
    }
    skip_whitespace(index);
    if (at_end(index)) {
      return fail(ErrorCode::ParseError, "unexpected end of JSON input");
    }
    const char c = text_[index];
    switch (c) {
      case '{':
        return parse_object(index, depth);
      case '[':
        return parse_array(index, depth);
      case '"':
        return parse_string(index);
      case 't':
        return parse_literal(index, "true", JsonValue::make_bool(true));
      case 'f':
        return parse_literal(index, "false", JsonValue::make_bool(false));
      case 'n':
        return parse_literal(index, "null", JsonValue::make_null());
      default:
        if (c == '-' || (c >= '0' && c <= '9')) {
          return parse_number(index);
        }
        return fail(ErrorCode::ParseError, "unexpected character where a JSON value was expected");
    }
  }

  Result<JsonValue> parse_literal(std::size_t& index, std::string_view literal,
                                  JsonValue value) {
    if (text_.size() - index < literal.size() ||
        text_.compare(index, literal.size(), literal) != 0) {
      return fail(ErrorCode::ParseError, "malformed JSON literal");
    }
    index += literal.size();
    return value;
  }

  Result<JsonValue> parse_object(std::size_t& index, std::size_t depth) {
    ++index;  // consume '{'
    JsonValue object = JsonValue::make_object();
    skip_whitespace(index);
    if (!at_end(index) && text_[index] == '}') {
      ++index;
      return object;
    }
    std::size_t members = 0;
    while (true) {
      skip_whitespace(index);
      if (at_end(index) || text_[index] != '"') {
        return fail(ErrorCode::ParseError, "object member name must be a JSON string");
      }
      auto key = parse_string(index);
      if (!key.has_value()) {
        return key.error();
      }
      const std::string key_text = std::move(key).value().as_string();
      if (object.find(key_text) != nullptr) {
        return fail(ErrorCode::ParseError, "duplicate object member name");
      }
      skip_whitespace(index);
      if (at_end(index) || text_[index] != ':') {
        return fail(ErrorCode::ParseError, "expected a colon after the object member name");
      }
      ++index;
      auto value = parse_value(index, depth + 1);
      if (!value.has_value()) {
        return value.error();
      }
      object.set(key_text, std::move(value).value());
      ++members;
      if (members > limits_.max_object_members) {
        return fail(ErrorCode::LimitExceeded,
                    "object member count exceeds the configured limit");
      }
      skip_whitespace(index);
      if (at_end(index)) {
        return fail(ErrorCode::ParseError, "unterminated JSON object");
      }
      if (text_[index] == ',') {
        ++index;
        continue;
      }
      if (text_[index] == '}') {
        ++index;
        return object;
      }
      return fail(ErrorCode::ParseError, "expected a comma or a closing brace in a JSON object");
    }
  }

  Result<JsonValue> parse_array(std::size_t& index, std::size_t depth) {
    ++index;  // consume '['
    JsonValue array = JsonValue::make_array();
    skip_whitespace(index);
    if (!at_end(index) && text_[index] == ']') {
      ++index;
      return array;
    }
    std::size_t elements = 0;
    while (true) {
      auto value = parse_value(index, depth + 1);
      if (!value.has_value()) {
        return value.error();
      }
      array.push(std::move(value).value());
      ++elements;
      if (elements > limits_.max_array_elements) {
        return fail(ErrorCode::LimitExceeded,
                    "array element count exceeds the configured limit");
      }
      skip_whitespace(index);
      if (at_end(index)) {
        return fail(ErrorCode::ParseError, "unterminated JSON array");
      }
      if (text_[index] == ',') {
        ++index;
        continue;
      }
      if (text_[index] == ']') {
        ++index;
        return array;
      }
      return fail(ErrorCode::ParseError, "expected a comma or a closing bracket in a JSON array");
    }
  }

  Result<JsonValue> parse_string(std::size_t& index) {
    ++index;  // consume opening quote
    std::string out;
    while (true) {
      if (at_end(index)) {
        return fail(ErrorCode::ParseError, "unterminated JSON string");
      }
      const char c = text_[index];
      if (c == '"') {
        ++index;
        return JsonValue::make_string(std::move(out));
      }
      if (static_cast<unsigned char>(c) < 0x20u) {
        return fail(ErrorCode::ParseError, "unescaped control character in a JSON string");
      }
      if (c != '\\') {
        out.push_back(c);
        ++index;
        if (out.size() > limits_.max_string_bytes) {
          return fail(ErrorCode::LimitExceeded, "JSON string exceeds the configured limit");
        }
        continue;
      }
      ++index;
      if (at_end(index)) {
        return fail(ErrorCode::ParseError, "unterminated escape sequence in a JSON string");
      }
      const char escape = text_[index];
      ++index;
      switch (escape) {
        case '"':
          out.push_back('"');
          break;
        case '\\':
          out.push_back('\\');
          break;
        case '/':
          out.push_back('/');
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u': {
          std::uint32_t code_point = 0;
          auto first = parse_hex4(index, code_point);
          if (!first.ok()) {
            return Result<JsonValue>(first.error());
          }
          if (code_point >= 0xD800u && code_point <= 0xDBFFu) {
            if (text_.size() - index < 2u || text_[index] != '\\' || text_[index + 1u] != 'u') {
              return fail(ErrorCode::ParseError,
                          "high surrogate is not followed by a low surrogate escape");
            }
            index += 2u;
            std::uint32_t low = 0;
            auto second = parse_hex4(index, low);
            if (!second.ok()) {
              return Result<JsonValue>(second.error());
            }
            if (low < 0xDC00u || low > 0xDFFFu) {
              return fail(ErrorCode::ParseError, "invalid low surrogate in a JSON string");
            }
            code_point = 0x10000u + ((code_point - 0xD800u) << 10u) + (low - 0xDC00u);
          } else if (code_point >= 0xDC00u && code_point <= 0xDFFFu) {
            return fail(ErrorCode::ParseError, "unpaired low surrogate in a JSON string");
          }
          append_utf8(out, code_point);
          break;
        }
        default:
          return fail(ErrorCode::ParseError, "unknown escape sequence in a JSON string");
      }
      if (out.size() > limits_.max_string_bytes) {
        return fail(ErrorCode::LimitExceeded, "JSON string exceeds the configured limit");
      }
    }
  }

  Status parse_hex4(std::size_t& index, std::uint32_t& out) {
    if (text_.size() - index < 4u) {
      return Status::failure(ErrorCode::ParseError, "truncated unicode escape in a JSON string");
    }
    std::uint32_t value = 0;
    for (int digit = 0; digit < 4; ++digit) {
      const char c = text_[index + static_cast<std::size_t>(digit)];
      std::uint32_t nibble = 0;
      if (c >= '0' && c <= '9') {
        nibble = static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        nibble = static_cast<std::uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        nibble = static_cast<std::uint32_t>(c - 'A' + 10);
      } else {
        return Status::failure(ErrorCode::ParseError,
                               "non-hexadecimal digit in a unicode escape");
      }
      value = (value << 4u) | nibble;
    }
    index += 4u;
    out = value;
    return Status::success();
  }

  Result<JsonValue> parse_number(std::size_t& index) {
    const std::size_t start = index;
    if (text_[index] == '-') {
      ++index;
    }
    if (at_end(index) || text_[index] < '0' || text_[index] > '9') {
      return fail(ErrorCode::ParseError, "malformed JSON number");
    }
    if (text_[index] == '0') {
      ++index;
      if (!at_end(index) && text_[index] >= '0' && text_[index] <= '9') {
        return fail(ErrorCode::ParseError, "JSON numbers must not have leading zeros");
      }
    } else {
      while (!at_end(index) && text_[index] >= '0' && text_[index] <= '9') {
        ++index;
      }
    }
    bool integral = true;
    if (!at_end(index) && text_[index] == '.') {
      integral = false;
      ++index;
      if (at_end(index) || text_[index] < '0' || text_[index] > '9') {
        return fail(ErrorCode::ParseError, "JSON number has no digits after the decimal point");
      }
      while (!at_end(index) && text_[index] >= '0' && text_[index] <= '9') {
        ++index;
      }
    }
    if (!at_end(index) && (text_[index] == 'e' || text_[index] == 'E')) {
      integral = false;
      ++index;
      if (!at_end(index) && (text_[index] == '+' || text_[index] == '-')) {
        ++index;
      }
      if (at_end(index) || text_[index] < '0' || text_[index] > '9') {
        return fail(ErrorCode::ParseError, "JSON number has no digits in its exponent");
      }
      while (!at_end(index) && text_[index] >= '0' && text_[index] <= '9') {
        ++index;
      }
    }
    const std::string_view span = text_.substr(start, index - start);
    if (!integral) {
      double parsed = 0.0;
      const auto result =
          std::from_chars(span.data(), span.data() + span.size(), parsed, std::chars_format::general);
      if (result.ec != std::errc{} || result.ptr != span.data() + span.size()) {
        return fail(ErrorCode::ParseError, "JSON number cannot be represented as a double");
      }
      if (!std::isfinite(parsed)) {
        return fail(ErrorCode::OutOfRange, "JSON number is not finite");
      }
      return JsonValue::make_double(parsed);
    }
    if (!span.empty() && span.front() == '-') {
      std::int64_t parsed = 0;
      const auto result = std::from_chars(span.data(), span.data() + span.size(), parsed);
      if (result.ec != std::errc{} || result.ptr != span.data() + span.size()) {
        return fail(ErrorCode::OutOfRange, "JSON integer is outside the signed 64-bit range");
      }
      return JsonValue::make_int(parsed);
    }
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(span.data(), span.data() + span.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != span.data() + span.size()) {
      return fail(ErrorCode::OutOfRange, "JSON integer is outside the unsigned 64-bit range");
    }
    return JsonValue::make_uint(parsed);
  }

  std::string_view text_{};
  JsonLimits limits_{};
  std::size_t nodes_{0};
};

}  // namespace

JsonValue JsonValue::make_null() { return JsonValue(); }

JsonValue JsonValue::make_bool(bool value) {
  JsonValue out;
  out.type_ = Type::Bool;
  out.bool_value_ = value;
  return out;
}

JsonValue JsonValue::make_int(std::int64_t value) {
  JsonValue out;
  out.type_ = Type::Int;
  out.int_value_ = value;
  return out;
}

JsonValue JsonValue::make_uint(std::uint64_t value) {
  JsonValue out;
  out.type_ = Type::Uint;
  out.uint_value_ = value;
  return out;
}

JsonValue JsonValue::make_double(double value) {
  JsonValue out;
  out.type_ = Type::Double;
  out.double_value_ = value;
  return out;
}

JsonValue JsonValue::make_string(std::string value) {
  JsonValue out;
  out.type_ = Type::String;
  out.string_value_ = std::move(value);
  return out;
}

JsonValue JsonValue::make_array() {
  JsonValue out;
  out.type_ = Type::Array;
  return out;
}

JsonValue JsonValue::make_object() {
  JsonValue out;
  out.type_ = Type::Object;
  return out;
}

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
  if (type_ != Type::Object) {
    return nullptr;
  }
  for (std::size_t index = 0; index < keys_.size(); ++index) {
    if (keys_[index] == key) {
      return &children_[index];
    }
  }
  return nullptr;
}

std::size_t JsonValue::size() const noexcept {
  switch (type_) {
    case Type::Array:
    case Type::Object:
      return children_.size();
    case Type::String:
      return string_value_.size();
    default:
      return 0;
  }
}

std::string_view JsonValue::key_at(std::size_t index) const noexcept {
  if (type_ != Type::Object || index >= keys_.size()) {
    return std::string_view{};
  }
  return keys_[index];
}

const JsonValue& JsonValue::value_at(std::size_t index) const noexcept {
  static const JsonValue kNull{};
  if (index >= children_.size()) {
    return kNull;
  }
  return children_[index];
}

void JsonValue::push(JsonValue value) {
  if (type_ == Type::Array) {
    children_.push_back(std::move(value));
  }
}

void JsonValue::set(std::string key, JsonValue value) {
  if (type_ != Type::Object) {
    return;
  }
  keys_.push_back(std::move(key));
  children_.push_back(std::move(value));
}

std::string_view JsonValue::type_name() const noexcept {
  switch (type_) {
    case Type::Null:
      return "null";
    case Type::Bool:
      return "boolean";
    case Type::Int:
    case Type::Uint:
      return "integer";
    case Type::Double:
      return "number";
    case Type::String:
      return "string";
    case Type::Array:
      return "array";
    case Type::Object:
      return "object";
  }
  return "unknown";
}

Result<JsonValue> parse_json(std::string_view text, const JsonLimits& limits) {
  if (text.size() > limits.max_document_bytes) {
    return Result<JsonValue>::failure(ErrorCode::LimitExceeded,
                                      "JSON document exceeds the configured byte limit");
  }
  Parser parser(text, limits);
  return parser.parse_document();
}

Result<JsonValue> parse_json_prefix(std::string_view text, const JsonLimits& limits,
                                    std::size_t& consumed) {
  if (text.size() > limits.max_document_bytes) {
    return Result<JsonValue>::failure(ErrorCode::LimitExceeded,
                                      "JSON document exceeds the configured byte limit");
  }
  Parser parser(text, limits);
  return parser.parse_prefix(consumed);
}

// ---------------------------------------------------------------------------
// JsonWriter
// ---------------------------------------------------------------------------

Status JsonWriter::fail(ErrorCode code, std::string message) {
  if (error_.code() == ErrorCode::Ok) {
    error_ = Error(code, std::move(message));
  }
  return Status(error_);
}

Status JsonWriter::ready() const {
  if (error_.code() != ErrorCode::Ok) {
    return Status(error_);
  }
  return Status::success();
}

void JsonWriter::newline_indent() {
  if (!pretty_) {
    return;
  }
  buffer_.push_back('\n');
  buffer_.append(stack_.size() * 2u, ' ');
}

Status JsonWriter::before_value() {
  if (error_.code() != ErrorCode::Ok) {
    return Status(error_);
  }
  if (stack_.empty()) {
    if (!buffer_.empty()) {
      return fail(ErrorCode::Internal, "a JSON document may contain only one top-level value");
    }
    awaiting_value_ = false;
    return Status::success();
  }
  if (stack_.back() == Frame::Object) {
    // The member separator is written together with the member name, so a value
    // inside an object must not add one of its own.
    if (!awaiting_value_) {
      return fail(ErrorCode::Internal, "a member value must follow a member name");
    }
    awaiting_value_ = false;
    return Status::success();
  }
  if (!has_items_.back()) {
    has_items_.back() = true;
  } else {
    buffer_.push_back(',');
  }
  newline_indent();
  awaiting_value_ = false;
  return Status::success();
}

Status JsonWriter::begin_object() {
  QOBS_TRY(before_value());
  buffer_.push_back('{');
  stack_.push_back(Frame::Object);
  has_items_.push_back(false);
  awaiting_value_ = false;
  return Status::success();
}

Status JsonWriter::end_object() {
  QOBS_TRY(ready());
  if (stack_.empty() || stack_.back() != Frame::Object) {
    return fail(ErrorCode::Internal, "no JSON object is open");
  }
  if (awaiting_value_) {
    return fail(ErrorCode::Internal, "a JSON object is missing the value of its last member");
  }
  const bool had_items = has_items_.back();
  stack_.pop_back();
  has_items_.pop_back();
  awaiting_value_ = false;
  if (had_items) {
    newline_indent();
  }
  buffer_.push_back('}');
  return Status::success();
}

Status JsonWriter::begin_array() {
  QOBS_TRY(before_value());
  buffer_.push_back('[');
  stack_.push_back(Frame::Array);
  has_items_.push_back(false);
  awaiting_value_ = false;
  return Status::success();
}

Status JsonWriter::end_array() {
  QOBS_TRY(ready());
  if (stack_.empty() || stack_.back() != Frame::Array) {
    return fail(ErrorCode::Internal, "no JSON array is open");
  }
  const bool had_items = has_items_.back();
  stack_.pop_back();
  has_items_.pop_back();
  awaiting_value_ = false;
  if (had_items) {
    newline_indent();
  }
  buffer_.push_back(']');
  return Status::success();
}

Status JsonWriter::key(std::string_view name) {
  QOBS_TRY(ready());
  if (stack_.empty() || stack_.back() != Frame::Object || awaiting_value_) {
    return fail(ErrorCode::Internal, "a member name is only valid directly inside an object");
  }
  if (!has_items_.back()) {
    has_items_.back() = true;
  } else {
    buffer_.push_back(',');
  }
  newline_indent();
  buffer_.append("\"");
  buffer_.append(text::escape_json(name));
  buffer_.append("\":");
  if (pretty_) {
    buffer_.push_back(' ');
  }
  awaiting_value_ = true;
  return Status::success();
}

Status JsonWriter::value_string(std::string_view value) {
  QOBS_TRY(before_value());
  buffer_.push_back('"');
  buffer_.append(text::escape_json(value));
  buffer_.push_back('"');
  return Status::success();
}

Status JsonWriter::value_u64(std::uint64_t value) {
  QOBS_TRY(before_value());
  buffer_.append(std::to_string(value));
  return Status::success();
}

Status JsonWriter::value_i64(std::int64_t value) {
  QOBS_TRY(before_value());
  buffer_.append(std::to_string(value));
  return Status::success();
}

Status JsonWriter::value_bool(bool value) {
  QOBS_TRY(before_value());
  buffer_.append(value ? "true" : "false");
  return Status::success();
}

Status JsonWriter::value_null() {
  QOBS_TRY(before_value());
  buffer_.append("null");
  return Status::success();
}

Status JsonWriter::value_fixed(std::uint64_t units, std::uint32_t scale) {
  QOBS_TRY(before_value());
  if (scale == 0u) {
    buffer_.append(std::to_string(units));
    return Status::success();
  }
  if (scale > 9u) {
    return fail(ErrorCode::InvalidArgument, "fixed-point scale must not exceed 9 digits");
  }
  std::uint64_t divisor = 1;
  for (std::uint32_t digit = 0; digit < scale; ++digit) {
    divisor *= 10u;
  }
  const std::uint64_t whole = units / divisor;
  const std::uint64_t fraction = units % divisor;
  buffer_.append(std::to_string(whole));
  buffer_.push_back('.');
  std::string fraction_text = std::to_string(fraction);
  while (fraction_text.size() < scale) {
    fraction_text.insert(fraction_text.begin(), '0');
  }
  buffer_.append(fraction_text);
  return Status::success();
}

Status JsonWriter::value_passthrough(std::string_view fragment) {
  QOBS_TRY(before_value());
  buffer_.append(fragment);
  return Status::success();
}

Status JsonWriter::member_string(std::string_view name, std::string_view value) {
  QOBS_TRY(key(name));
  return value_string(value);
}

Status JsonWriter::member_u64(std::string_view name, std::uint64_t value) {
  QOBS_TRY(key(name));
  return value_u64(value);
}

Status JsonWriter::member_i64(std::string_view name, std::int64_t value) {
  QOBS_TRY(key(name));
  return value_i64(value);
}

Status JsonWriter::member_bool(std::string_view name, bool value) {
  QOBS_TRY(key(name));
  return value_bool(value);
}

}  // namespace qobs
