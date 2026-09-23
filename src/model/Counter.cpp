#include "qobs/model/Counter.hpp"

#include "qobs/core/Checked.hpp"

namespace qobs {
namespace {

constexpr std::uint64_t kNanosPerSecond = 1000000000ull;

std::string describe_gap(Nanos gap_ns) {
  return std::to_string(static_cast<long long>(gap_ns)) + "ns";
}

/// Upper bound on how far the counter could have advanced inside the gap, when
/// the policy supplies a rate bound.
std::optional<std::uint64_t> max_advance_in_gap(const ContinuityPolicy& policy, Nanos gap_ns) {
  if (!policy.max_rate_per_second.has_value() || gap_ns < 0) {
    return std::nullopt;
  }
  const auto scaled =
      checked::mul_u64(*policy.max_rate_per_second, static_cast<std::uint64_t>(gap_ns));
  if (!scaled.has_value()) {
    return std::nullopt;
  }
  return *scaled / kNanosPerSecond;
}

}  // namespace

std::string_view to_string(CounterWidth width) noexcept {
  switch (width) {
    case CounterWidth::Bits32:
      return "32";
    case CounterWidth::Bits64:
      return "64";
  }
  return "unknown";
}

std::string_view to_string(CounterContinuity value) noexcept {
  switch (value) {
    case CounterContinuity::FirstObservation:
      return "first_observation";
    case CounterContinuity::Continuous:
      return "continuous";
    case CounterContinuity::Unchanged:
      return "unchanged";
    case CounterContinuity::Wrapped:
      return "wrapped";
    case CounterContinuity::Reset:
      return "reset";
    case CounterContinuity::Discontinuous:
      return "discontinuous";
    case CounterContinuity::AmbiguousWrap:
      return "ambiguous_wrap";
    case CounterContinuity::Undetermined:
      return "undetermined";
  }
  return "unknown";
}

bool continuity_yields_known_delta(CounterContinuity value) noexcept {
  return value == CounterContinuity::Continuous || value == CounterContinuity::Unchanged ||
         value == CounterContinuity::Wrapped;
}

CounterUpdate update_counter(CounterState& state, std::uint64_t raw, const ObservationTime& observed,
                             const ReceiveTime& received, const ContinuityPolicy& policy) {
  CounterUpdate update;

  if (!state.initialized) {
    state.initialized = true;
    state.width = policy.width;
    state.last_raw = raw;
    state.last_observed = observed;
    state.last_received = received;
    update.continuity = CounterContinuity::FirstObservation;
    update.delta_known = false;
    update.reason = "first observation of this counter establishes the baseline";
    return update;
  }

  if (wraps_within_64_bits(policy.width) && raw >= counter_modulus(policy.width)) {
    // A reading that cannot exist for a counter of this width is refused
    // outright. Continuing would make the backward-distance arithmetic
    // meaningless, and a meaningless subtraction here would silently corrupt
    // every later total.
    state.last_observed = observed;
    state.last_received = received;
    update.continuity = CounterContinuity::Undetermined;
    update.reason = "the reported value exceeds the width of the counter it belongs to";
    state.unknown_deltas += 1u;
    return update;
  }

  const std::uint64_t previous = state.last_raw;
  const ObservationTime previous_observed = state.last_observed;
  const ReceiveTime previous_received = state.last_received;

  bool gap_known = true;
  Nanos gap_ns = 0;
  bool order_anomaly = false;
  const TimeComparison comparison = compare_observation_times(previous_observed, observed);
  if (comparison.comparable()) {
    gap_ns = observed.ns - previous_observed.ns;
  } else {
    gap_ns = received.steady.ns - previous_received.steady.ns;
  }
  if (gap_ns < 0) {
    order_anomaly = true;
    gap_known = false;
  }

  state.last_raw = raw;
  state.last_observed = observed;
  state.last_received = received;

  if (order_anomaly) {
    update.continuity = CounterContinuity::Undetermined;
    update.reason = "observation time precedes the previous sample; no delta is claimed";
    state.unknown_deltas += 1u;
    return update;
  }

  if (gap_known && policy.max_sample_gap_ns > 0 && gap_ns > policy.max_sample_gap_ns) {
    update.continuity = CounterContinuity::Discontinuous;
    update.reason = "gap of " + describe_gap(gap_ns) + " exceeds the continuity limit of " +
                    describe_gap(policy.max_sample_gap_ns);
    state.discontinuities += 1u;
    state.unknown_deltas += 1u;
    return update;
  }

  if (raw == previous) {
    update.continuity = CounterContinuity::Unchanged;
    update.delta_known = true;
    update.delta = 0;
    state.known_deltas += 1u;
    return update;
  }

  const std::uint64_t modulus = counter_modulus(policy.width);
  const bool wraps = wraps_within_64_bits(policy.width);
  const std::optional<std::uint64_t> max_advance = max_advance_in_gap(policy, gap_ns);

  if (raw > previous) {
    const std::uint64_t delta = raw - previous;
    if (max_advance.has_value() && wraps) {
      const auto extra_wrap = checked::add_u64(delta, modulus);
      if (extra_wrap.has_value() && *max_advance >= *extra_wrap) {
        update.continuity = CounterContinuity::AmbiguousWrap;
        update.reason = "the counter could have wrapped at least once inside the gap; no delta is claimed";
        state.ambiguous += 1u;
        state.unknown_deltas += 1u;
        return update;
      }
    }
    update.continuity = CounterContinuity::Continuous;
    update.delta_known = true;
    update.delta = delta;
    state.known_total += delta;
    state.known_deltas += 1u;
    return update;
  }

  const std::uint64_t backwards = previous - raw;
  if (wraps && backwards > modulus / 2u) {
    const std::uint64_t delta = modulus - previous + raw;
    if (max_advance.has_value()) {
      const auto extra_wrap = checked::add_u64(delta, modulus);
      if (extra_wrap.has_value() && *max_advance >= *extra_wrap) {
        update.continuity = CounterContinuity::AmbiguousWrap;
        update.reason = "the counter could have wrapped more than once inside the gap; no delta is claimed";
        state.ambiguous += 1u;
        state.unknown_deltas += 1u;
        return update;
      }
    }
    update.continuity = CounterContinuity::Wrapped;
    update.delta_known = true;
    update.delta = delta;
    update.regressed = true;
    state.wraps += 1u;
    state.known_total += delta;
    state.known_deltas += 1u;
    return update;
  }

  if (policy.reset_detection_enabled) {
    update.continuity = CounterContinuity::Reset;
    update.regressed = true;
    update.reason = "counter decreased by " + std::to_string(backwards) +
                    " which is too small for a wrap of a " +
                    std::string(to_string(policy.width)) + "-bit counter";
    state.resets += 1u;
    state.unknown_deltas += 1u;
    return update;
  }

  update.continuity = CounterContinuity::Undetermined;
  update.regressed = true;
  update.reason = "counter decreased and reset detection is disabled; no delta is claimed";
  state.unknown_deltas += 1u;
  return update;
}

}  // namespace qobs
