#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "qobs/core/Result.hpp"
#include "qobs/core/Time.hpp"

namespace qobs {

/// Counters on network devices are finite-width and wrap; they also reset when
/// a device reloads or a queue is recreated. This module makes every one of
/// those outcomes an explicit, named result instead of an implicit subtraction.
enum class CounterWidth : std::uint8_t { Bits32 = 32, Bits64 = 64 };

[[nodiscard]] constexpr std::uint64_t counter_modulus(CounterWidth width) noexcept {
  return width == CounterWidth::Bits32 ? 0x100000000ull : 0ull;  // 0 means "no wrap within range"
}

[[nodiscard]] std::string_view to_string(CounterWidth width) noexcept;
[[nodiscard]] constexpr bool wraps_within_64_bits(CounterWidth width) noexcept {
  return width == CounterWidth::Bits32;
}

enum class CounterContinuity : std::uint8_t {
  /// No previous observation exists for this counter.
  FirstObservation,
  /// The counter advanced by a known amount.
  Continuous,
  /// The counter advanced by zero.
  Unchanged,
  /// The counter decreased by more than half of its range and was interpreted
  /// as a single wrap.
  Wrapped,
  /// The counter decreased by at most half of its range and was interpreted as
  /// a restart.
  Reset,
  /// Too much time passed between observations to make any claim about the
  /// delta.
  Discontinuous,
  /// More than one wrap could have happened inside the gap. No delta is
  /// claimed: inventing one would be fabricating evidence.
  AmbiguousWrap,
  /// The counter is not usable for a delta for another stated reason.
  Undetermined,
};

[[nodiscard]] std::string_view to_string(CounterContinuity value) noexcept;
[[nodiscard]] bool continuity_yields_known_delta(CounterContinuity value) noexcept;

/// Policy needed to interpret a counter. Every field is explicit because the
/// interpretation must be reproducible.
struct ContinuityPolicy {
  CounterWidth width{CounterWidth::Bits32};
  /// A gap larger than this between consecutive observations makes the delta
  /// unknowable.
  Nanos max_sample_gap_ns{30000000000LL};  // 30 seconds
  /// Upper bound on how fast the counter can physically advance. When set, it
  /// is used to decide whether more than one wrap could fit inside a gap.
  std::optional<std::uint64_t> max_rate_per_second{};
  /// When false, a backwards movement is never classified as a reset and is
  /// reported as Undetermined instead.
  bool reset_detection_enabled{true};

  friend bool operator==(const ContinuityPolicy&, const ContinuityPolicy&) = default;
};

struct CounterState {
  bool initialized{false};
  CounterWidth width{CounterWidth::Bits32};
  std::uint64_t last_raw{0};
  ObservationTime last_observed{};
  ReceiveTime last_received{};
  /// Sum of every delta this runtime was able to justify. Gaps, resets and
  /// ambiguous wraps contribute nothing, so the total never invents progress.
  std::uint64_t known_total{0};
  std::uint64_t wraps{0};
  std::uint64_t resets{0};
  std::uint64_t ambiguous{0};
  std::uint64_t discontinuities{0};
  /// Number of samples that produced a known delta.
  std::uint64_t known_deltas{0};
  /// Number of samples whose delta could not be established.
  std::uint64_t unknown_deltas{0};
};

/// Outcome of folding one raw counter reading into a CounterState.
struct CounterUpdate {
  CounterContinuity continuity{CounterContinuity::Undetermined};
  bool delta_known{false};
  std::uint64_t delta{0};
  /// True when the counter value moved backwards.
  bool regressed{false};
  std::string reason{};
};

/// Evaluate a raw reading and update the state in place.
///
/// The observation time is used for gap detection only when both the previous
/// and the current observation carry the same, known clock domain. Otherwise
/// gap detection falls back to the receive clock, which is always monotonic and
/// always available.
[[nodiscard]] CounterUpdate update_counter(CounterState& state, std::uint64_t raw,
                                           const ObservationTime& observed,
                                           const ReceiveTime& received,
                                           const ContinuityPolicy& policy);

/// A counter delta as carried in history records.
struct CounterDelta {
  CounterContinuity continuity{CounterContinuity::Undetermined};
  bool known{false};
  std::uint64_t delta{0};

  friend bool operator==(const CounterDelta&, const CounterDelta&) = default;
};

}  // namespace qobs
