#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace qobs {

/// Every fallible operation in Queue Observatory reports one of these codes.
/// Callers are expected to branch on the code, not to parse the message.
enum class ErrorCode : int {
  Ok = 0,
  InvalidArgument,
  OutOfRange,
  Overflow,
  NotSupported,
  NotFound,
  AlreadyExists,
  IntegrityFailure,
  VersionMismatch,
  ParseError,
  LimitExceeded,
  Fenced,
  Stale,
  Conflict,
  Incomplete,
  Unknown,
  Cancelled,
  NotRunning,
  Busy,
  IoError,
  Internal,
};

[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;
[[nodiscard]] bool is_failure(ErrorCode code) noexcept;

/// A code plus a human readable, deterministic message.
class Error {
 public:
  Error() = default;
  Error(ErrorCode code, std::string message) : code_(code), message_(std::move(message)) {}

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] const std::string& message() const noexcept { return message_; }
  [[nodiscard]] std::string to_string() const;

 private:
  ErrorCode code_{ErrorCode::Ok};
  std::string message_{};
};

/// A result with no payload.
class Status {
 public:
  Status() = default;
  Status(const Error& error) : error_(error) {}
  Status(ErrorCode code, std::string message) : error_(code, std::move(message)) {}

  [[nodiscard]] static Status success() noexcept { return Status(); }
  [[nodiscard]] static Status failure(ErrorCode code, std::string message) {
    return Status(code, std::move(message));
  }

  [[nodiscard]] bool ok() const noexcept { return error_.code() == ErrorCode::Ok; }
  [[nodiscard]] ErrorCode code() const noexcept { return error_.code(); }
  [[nodiscard]] const std::string& message() const noexcept { return error_.message(); }
  [[nodiscard]] const Error& error() const noexcept { return error_; }
  [[nodiscard]] std::string to_string() const { return error_.to_string(); }

 private:
  Error error_{};
};

/// A result carrying a value on success and an Error on failure.
template <class T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}  // NOLINT(google-explicit-constructor)
  Result(const Error& error) : error_(error) {}  // NOLINT(google-explicit-constructor)
  Result(ErrorCode code, std::string message) : error_(code, std::move(message)) {}

  [[nodiscard]] static Result failure(ErrorCode code, std::string message) {
    return Result(Error(code, std::move(message)));
  }

  [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] T& value() & { return *value_; }
  [[nodiscard]] const T& value() const& { return *value_; }
  [[nodiscard]] T&& value() && { return std::move(*value_); }

  [[nodiscard]] T* operator->() { return &*value_; }
  [[nodiscard]] const T* operator->() const { return &*value_; }
  [[nodiscard]] T& operator*() & { return *value_; }
  [[nodiscard]] const T& operator*() const& { return *value_; }
  [[nodiscard]] const Error& error() const noexcept { return error_; }

  /// Value or the supplied fallback; used only where a conservative default is
  /// explicitly documented at the call site.
  [[nodiscard]] T value_or(T fallback) const {
    return value_.has_value() ? *value_ : std::move(fallback);
  }

 private:
  std::optional<T> value_{};
  Error error_{};
};

#define QOBS_TRY(expr)                        \
  do {                                        \
    ::qobs::Status qobs_try_status_ = (expr); \
    if (!qobs_try_status_.ok()) {             \
      return qobs_try_status_;                \
    }                                         \
  } while (false)

/// Propagate a Status failure out of a function that returns a Result. The
/// error is returned rather than a value, so the degenerate "empty value, no
/// error" state cannot be produced by this macro.
#define QOBS_TRY_RESULT(expr)                    \
  do {                                           \
    ::qobs::Status qobs_try_st_ = (expr);        \
    if (!qobs_try_st_.ok()) {                    \
      return qobs_try_st_.error();               \
    }                                            \
  } while (false)

#define QOBS_CONCAT_INNER(a, b) a##b
#define QOBS_CONCAT(a, b) QOBS_CONCAT_INNER(a, b)

/// Bind the value of a Result, returning its Error from the enclosing function
/// when the Result holds no value. The internal name is line-unique so that
/// several uses may appear in one scope.
#define QOBS_TRY_ASSIGN(name, expr)                                            \
  auto QOBS_CONCAT(qobs_try_result_, __LINE__) = (expr);                       \
  if (!QOBS_CONCAT(qobs_try_result_, __LINE__).has_value()) {                  \
    return QOBS_CONCAT(qobs_try_result_, __LINE__).error();                    \
  }                                                                            \
  auto name = std::move(QOBS_CONCAT(qobs_try_result_, __LINE__)).value()

}  // namespace qobs
