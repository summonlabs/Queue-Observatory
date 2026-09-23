#include "qobs/core/Result.hpp"

namespace qobs {

std::string_view to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok:
      return "ok";
    case ErrorCode::InvalidArgument:
      return "invalid_argument";
    case ErrorCode::OutOfRange:
      return "out_of_range";
    case ErrorCode::Overflow:
      return "overflow";
    case ErrorCode::NotSupported:
      return "not_supported";
    case ErrorCode::NotFound:
      return "not_found";
    case ErrorCode::AlreadyExists:
      return "already_exists";
    case ErrorCode::IntegrityFailure:
      return "integrity_failure";
    case ErrorCode::VersionMismatch:
      return "version_mismatch";
    case ErrorCode::ParseError:
      return "parse_error";
    case ErrorCode::LimitExceeded:
      return "limit_exceeded";
    case ErrorCode::Fenced:
      return "fenced";
    case ErrorCode::Stale:
      return "stale";
    case ErrorCode::Conflict:
      return "conflict";
    case ErrorCode::Incomplete:
      return "incomplete";
    case ErrorCode::Unknown:
      return "unknown";
    case ErrorCode::Cancelled:
      return "cancelled";
    case ErrorCode::NotRunning:
      return "not_running";
    case ErrorCode::Busy:
      return "busy";
    case ErrorCode::IoError:
      return "io_error";
    case ErrorCode::Internal:
      return "internal";
  }
  return "unmapped";
}

bool is_failure(ErrorCode code) noexcept { return code != ErrorCode::Ok; }

std::string Error::to_string() const {
  std::string out;
  out.reserve(message_.size() + 24u);
  out.append(qobs::to_string(code_));
  out.append(": ");
  out.append(message_);
  return out;
}

}  // namespace qobs
