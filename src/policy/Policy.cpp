#include "qobs/policy/Policy.hpp"

#include "qobs/core/Checked.hpp"

namespace qobs {

std::string_view to_string(PressureState state) noexcept {
  switch (state) {
    case PressureState::Unknown:
      return "unknown";
    case PressureState::Idle:
      return "idle";
    case PressureState::Normal:
      return "normal";
    case PressureState::Elevated:
      return "elevated";
    case PressureState::Pressured:
      return "pressured";
    case PressureState::Saturated:
      return "saturated";
    case PressureState::Dropping:
      return "dropping";
    case PressureState::Paused:
      return "paused";
    case PressureState::Stale:
      return "stale";
    case PressureState::Conflicting:
      return "conflicting";
  }
  return "unknown";
}

std::optional<PressureState> parse_pressure_state(std::string_view text) noexcept {
  constexpr PressureState kStates[kPressureStateCount] = {
      PressureState::Unknown, PressureState::Idle,        PressureState::Normal,
      PressureState::Elevated, PressureState::Pressured,  PressureState::Saturated,
      PressureState::Dropping, PressureState::Paused,     PressureState::Stale,
      PressureState::Conflicting};
  for (const PressureState state : kStates) {
    if (to_string(state) == text) {
      return state;
    }
  }
  return std::nullopt;
}

std::uint32_t pressure_severity_rank(PressureState state) noexcept {
  switch (state) {
    case PressureState::Unknown:
      return 5;
    case PressureState::Idle:
      return 0;
    case PressureState::Normal:
      return 1;
    case PressureState::Elevated:
      return 2;
    case PressureState::Pressured:
      return 3;
    case PressureState::Saturated:
      return 4;
    case PressureState::Paused:
      return 6;
    case PressureState::Dropping:
      return 7;
    case PressureState::Stale:
      return 8;
    case PressureState::Conflicting:
      return 9;
  }
  return 5;
}

std::string_view to_string(PressureBasisKind kind) noexcept {
  switch (kind) {
    case PressureBasisKind::None:
      return "none";
    case PressureBasisKind::CellsAgainstThreshold:
      return "cells_against_threshold";
    case PressureBasisKind::PacketsAgainstDepth:
      return "packets_against_depth";
    case PressureBasisKind::Absolute:
      return "absolute";
  }
  return "none";
}

const PressurePolicy& default_pressure_policy() {
  static const PressurePolicy policy{};
  return policy;
}

Status validate_policy(const PressurePolicy& policy) {
  if (policy.version == 0u) {
    return Status::failure(ErrorCode::InvalidArgument, "policy version must be non-zero");
  }
  if (policy.name.empty()) {
    return Status::failure(ErrorCode::InvalidArgument, "policy name must not be empty");
  }
  if (policy.required_fields == 0u && policy.required_any_fields == 0u) {
    return Status::failure(
        ErrorCode::InvalidArgument,
        "policy must require at least one sample field, either individually or as one of a set");
  }
  if (policy.evaluation_window_ns <= 0) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "policy evaluation window must be a positive duration");
  }

  const RelativeThresholds& relative = policy.relative;
  if (relative.elevated_permille == 0u || relative.elevated_permille > 1000u) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "elevated_permille must be within 1..1000");
  }
  if (relative.pressured_permille > 1000u || relative.saturated_permille > 1000u) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "relative thresholds must not exceed 1000 permille");
  }
  if (relative.elevated_permille > relative.pressured_permille ||
      relative.pressured_permille > relative.saturated_permille) {
    return Status::failure(
        ErrorCode::InvalidArgument,
        "relative thresholds must be non-decreasing from elevated to saturated");
  }

  const PressureThresholds& absolute = policy.absolute;
  if (absolute.elevated_at.has_value() && absolute.pressured_at.has_value() &&
      *absolute.pressured_at < *absolute.elevated_at) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "absolute pressured threshold must not be below the elevated one");
  }
  if (absolute.pressured_at.has_value() && absolute.saturated_at.has_value() &&
      *absolute.saturated_at < *absolute.pressured_at) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "absolute saturated threshold must not be below the pressured one");
  }
  if (absolute.elevated_at.has_value() && absolute.saturated_at.has_value() &&
      *absolute.saturated_at < *absolute.elevated_at) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "absolute saturated threshold must not be below the elevated one");
  }

  if (policy.freshness.fresh_within_ns <= 0 || policy.freshness.aging_within_ns < 0 ||
      policy.freshness.stale_within_ns < 0) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "freshness boundaries must be non-negative and ordered");
  }
  if (policy.freshness.fresh_within_ns > policy.freshness.aging_within_ns ||
      policy.freshness.aging_within_ns > policy.freshness.stale_within_ns) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "freshness boundaries must be non-decreasing");
  }

  if (policy.continuity.max_sample_gap_ns < 0) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "continuity gap limit must not be negative");
  }

  const MicroburstPolicy& microburst = policy.microburst;
  if (microburst.window_ns <= 0 || microburst.bucket_ns <= 0) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "microburst window and bucket must be positive durations");
  }
  if (microburst.bucket_ns > microburst.window_ns) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "microburst bucket must not be longer than the window");
  }
  if (microburst.max_buckets == 0u) {
    return Status::failure(ErrorCode::InvalidArgument, "microburst bucket budget must be non-zero");
  }
  const auto requested_buckets = checked::div_ceil_u64(
      static_cast<std::uint64_t>(microburst.window_ns),
      static_cast<std::uint64_t>(microburst.bucket_ns));
  if (!requested_buckets.has_value()) {
    return Status::failure(ErrorCode::Overflow, "microburst bucket count overflows");
  }
  if (*requested_buckets > static_cast<std::uint64_t>(microburst.max_buckets)) {
    return Status::failure(ErrorCode::LimitExceeded,
                           "microburst window needs more buckets than the policy allows");
  }
  if (microburst.min_covered_buckets == 0u) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "microburst requires at least one covered bucket");
  }
  if (static_cast<std::uint64_t>(microburst.min_covered_buckets) > *requested_buckets) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "microburst coverage requirement exceeds the bucket count");
  }

  if (policy.conflict.relative_permille > 1000u) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "conflict relative tolerance must not exceed 1000 permille");
  }
  if (policy.conflict.window_ns <= 0) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "conflict comparison window must be a positive duration");
  }

  const ContentionPolicy& contention = policy.contention;
  if (contention.window_ns <= 0) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "contention window must be a positive duration");
  }
  if (!contention.count_pressured && !contention.count_saturated && !contention.count_dropping &&
      !contention.count_paused) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "contention policy must count at least one state");
  }
  return Status::success();
}

Result<std::uint64_t> permille_of(std::uint64_t limit, std::uint32_t permille) noexcept {
  if (permille > 1000u) {
    return Result<std::uint64_t>::failure(ErrorCode::OutOfRange,
                                          "permille value must not exceed 1000");
  }
  const auto scaled = checked::mul_u64(limit, static_cast<std::uint64_t>(permille));
  if (!scaled.has_value()) {
    return Result<std::uint64_t>::failure(
        ErrorCode::Overflow, "scaling a limit by a permille factor overflowed");
  }
  // Ceiling division: a threshold is reached as soon as the value is at least
  // the rounded-up fraction of the limit. Rounding up keeps the classification
  // strict rather than optimistic.
  const auto rounded = checked::div_ceil_u64(*scaled, 1000ull);
  if (!rounded.has_value()) {
    return Result<std::uint64_t>::failure(ErrorCode::Internal, "permille division failed");
  }
  return *rounded;
}

}  // namespace qobs
