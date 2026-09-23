#include "support/TestHarness.hpp"

#include "qobs/model/Counter.hpp"

using namespace qobs;

namespace {

CounterState fresh_state() { return CounterState{}; }

ObservationTime observed(Nanos ns) {
  ObservationTime time;
  time.ns = ns;
  time.domain = ClockDomainId(std::string("d"));
  return time;
}

ReceiveTime received(Nanos ns) { return ReceiveTime{SteadyTime{ns}, WallTime{ns}}; }

ContinuityPolicy policy32() {
  ContinuityPolicy policy;
  policy.width = CounterWidth::Bits32;
  policy.max_sample_gap_ns = 1000000000LL;
  return policy;
}

}  // namespace

QOBS_TEST(counter, first_observation_establishes_baseline) {
  CounterState state = fresh_state();
  const CounterUpdate update =
      update_counter(state, 500u, observed(0), received(0), policy32());
  QOBS_CHECK_EQ(update.continuity, CounterContinuity::FirstObservation);
  QOBS_CHECK(!update.delta_known);
  QOBS_CHECK_EQ(state.known_total, 0u);
  QOBS_CHECK(state.initialized);
}

QOBS_TEST(counter, continuous_advance_is_summed) {
  CounterState state = fresh_state();
  const ContinuityPolicy policy = policy32();
  (void)update_counter(state, 100u, observed(0), received(0), policy);
  const CounterUpdate update = update_counter(state, 150u, observed(100), received(100), policy);
  QOBS_CHECK_EQ(update.continuity, CounterContinuity::Continuous);
  QOBS_CHECK(update.delta_known);
  QOBS_CHECK_EQ(update.delta, 50u);
  QOBS_CHECK_EQ(state.known_total, 50u);
  QOBS_CHECK_EQ(state.known_deltas, 1u);
}

QOBS_TEST(counter, unchanged_reports_zero_delta) {
  CounterState state = fresh_state();
  const ContinuityPolicy policy = policy32();
  (void)update_counter(state, 7u, observed(0), received(0), policy);
  const CounterUpdate update = update_counter(state, 7u, observed(10), received(10), policy);
  QOBS_CHECK_EQ(update.continuity, CounterContinuity::Unchanged);
  QOBS_CHECK(update.delta_known);
  QOBS_CHECK_EQ(update.delta, 0u);
}

QOBS_TEST(counter, wrap_is_interpreted_and_counted) {
  CounterState state = fresh_state();
  const ContinuityPolicy policy = policy32();
  (void)update_counter(state, 0xFFFFFFFEu, observed(0), received(0), policy);
  const CounterUpdate update =
      update_counter(state, 3u, observed(100), received(100), policy);
  QOBS_CHECK_EQ(update.continuity, CounterContinuity::Wrapped);
  QOBS_CHECK(update.delta_known);
  QOBS_CHECK_EQ(update.delta, 5u);
  QOBS_CHECK_EQ(state.wraps, 1u);
  QOBS_CHECK_EQ(state.known_total, 5u);
}

QOBS_TEST(counter, small_backwards_move_is_a_reset) {
  CounterState state = fresh_state();
  const ContinuityPolicy policy = policy32();
  (void)update_counter(state, 1000u, observed(0), received(0), policy);
  const CounterUpdate update = update_counter(state, 10u, observed(100), received(100), policy);
  QOBS_CHECK_EQ(update.continuity, CounterContinuity::Reset);
  QOBS_CHECK(!update.delta_known);
  QOBS_CHECK_EQ(state.resets, 1u);
  QOBS_CHECK_EQ(state.known_total, 0u);
  QOBS_CHECK_EQ(state.unknown_deltas, 1u);
}

QOBS_TEST(counter, reset_detection_can_be_disabled) {
  CounterState state = fresh_state();
  ContinuityPolicy policy = policy32();
  policy.reset_detection_enabled = false;
  (void)update_counter(state, 1000u, observed(0), received(0), policy);
  const CounterUpdate update = update_counter(state, 10u, observed(100), received(100), policy);
  QOBS_CHECK_EQ(update.continuity, CounterContinuity::Undetermined);
  QOBS_CHECK(!update.delta_known);
}

QOBS_TEST(counter, gap_beyond_the_limit_yields_no_claim) {
  CounterState state = fresh_state();
  const ContinuityPolicy policy = policy32();
  (void)update_counter(state, 100u, observed(0), received(0), policy);
  const CounterUpdate update =
      update_counter(state, 200u, observed(2000000000LL), received(2000000000LL), policy);
  QOBS_CHECK_EQ(update.continuity, CounterContinuity::Discontinuous);
  QOBS_CHECK(!update.delta_known);
  QOBS_CHECK_EQ(state.discontinuities, 1u);
  QOBS_CHECK_EQ(state.known_total, 0u);
}

QOBS_TEST(counter, multiple_possible_wraps_are_ambiguous) {
  CounterState state = fresh_state();
  ContinuityPolicy policy = policy32();
  // A rate bound large enough that a full extra wrap could fit inside the gap.
  policy.max_rate_per_second = 10000000000u;
  (void)update_counter(state, 10u, observed(0), received(0), policy);
  const CounterUpdate update =
      update_counter(state, 20u, observed(1000000000LL), received(1000000000LL), policy);
  QOBS_CHECK_EQ(update.continuity, CounterContinuity::AmbiguousWrap);
  QOBS_CHECK(!update.delta_known);
  QOBS_CHECK_EQ(state.ambiguous, 1u);
  QOBS_CHECK_EQ(state.known_total, 0u);
}

QOBS_TEST(counter, wrap_is_unambiguous_without_a_rate_bound) {
  CounterState state = fresh_state();
  ContinuityPolicy policy = policy32();
  (void)update_counter(state, 10u, observed(0), received(0), policy);
  const CounterUpdate update =
      update_counter(state, 20u, observed(1000000000LL), received(1000000000LL), policy);
  QOBS_CHECK_EQ(update.continuity, CounterContinuity::Continuous);
  QOBS_CHECK_EQ(update.delta, 10u);
}

QOBS_TEST(counter, sixty_four_bit_decrease_is_never_a_wrap) {
  CounterState state = fresh_state();
  ContinuityPolicy policy;
  policy.width = CounterWidth::Bits64;
  (void)update_counter(state, 1ull << 40u, observed(0), received(0), policy);
  const CounterUpdate update =
      update_counter(state, 1u, observed(100), received(100), policy);
  QOBS_CHECK_EQ(update.continuity, CounterContinuity::Reset);
  QOBS_CHECK(!update.delta_known);
}

QOBS_TEST(counter, backwards_observation_time_claims_nothing) {
  CounterState state = fresh_state();
  const ContinuityPolicy policy = policy32();
  (void)update_counter(state, 100u, observed(1000), received(1000), policy);
  const CounterUpdate update = update_counter(state, 200u, observed(500), received(500), policy);
  QOBS_CHECK_EQ(update.continuity, CounterContinuity::Undetermined);
  QOBS_CHECK(!update.delta_known);
  QOBS_CHECK_EQ(state.known_total, 0u);
}

QOBS_TEST(counter, different_clock_domains_fall_back_to_receive_time) {
  CounterState state = fresh_state();
  ContinuityPolicy policy = policy32();
  ObservationTime first = observed(0);
  (void)update_counter(state, 100u, first, received(0), policy);
  ObservationTime second = observed(5);
  second.domain = ClockDomainId(std::string("other"));
  const CounterUpdate update = update_counter(state, 150u, second, received(100), policy);
  QOBS_CHECK_EQ(update.continuity, CounterContinuity::Continuous);
  QOBS_CHECK_EQ(update.delta, 50u);
}

QOBS_TEST(counter, known_total_never_invents_progress) {
  CounterState state = fresh_state();
  const ContinuityPolicy policy = policy32();
  (void)update_counter(state, 1000u, observed(0), received(0), policy);
  (void)update_counter(state, 1100u, observed(10), received(10), policy);
  QOBS_CHECK_EQ(state.known_total, 100u);
  (void)update_counter(state, 5u, observed(20), received(20), policy);  // reset
  QOBS_CHECK_EQ(state.known_total, 100u);
  // A gap longer than the continuity limit claims nothing at all, even though
  // the raw value moved forward.
  const CounterUpdate gapped =
      update_counter(state, 2000000000LL, observed(3000000000LL), received(3000000000LL), policy);
  QOBS_CHECK_EQ(gapped.continuity, CounterContinuity::Discontinuous);
  QOBS_CHECK(!gapped.delta_known);
  QOBS_CHECK_EQ(state.known_total, 100u);
  QOBS_CHECK_EQ(update_counter(state, 1u, observed(3000000100LL), received(3000000100LL), policy)
                    .continuity,
                CounterContinuity::Reset);
}
