#include "support/TestHarness.hpp"

#include <vector>

#include "qobs/store/Window.hpp"

using namespace qobs;

namespace {

WindowSpec spec(Nanos duration, Nanos bucket, std::uint32_t min_covered,
                std::size_t max_buckets) {
  WindowSpec value;
  value.name = "test";
  value.duration_ns = duration;
  value.bucket_ns = bucket;
  value.min_covered_buckets = min_covered;
  value.max_buckets = max_buckets;
  return value;
}

WindowObservation observation(Nanos at, std::uint64_t occupancy, bool known_enqueue = true,
                              std::uint64_t enqueues = 1) {
  WindowObservation value;
  value.steady_ns = at;
  value.has_occupancy = true;
  value.occupancy = occupancy;
  value.enqueues.known = known_enqueue;
  value.enqueues.delta = enqueues;
  value.drops.known = true;
  value.drops.delta = 0;
  value.establishes_current = true;
  return value;
}

}  // namespace

QOBS_TEST(window, bucket_alignment_floors_towards_negative_infinity) {
  QOBS_CHECK_EQ(align_bucket(0, 100), 0);
  QOBS_CHECK_EQ(align_bucket(99, 100), 0);
  QOBS_CHECK_EQ(align_bucket(100, 100), 100);
  QOBS_CHECK_EQ(align_bucket(-1, 100), -100);
  QOBS_CHECK_EQ(align_bucket(-100, 100), -100);
  QOBS_CHECK_EQ(align_bucket(-101, 100), -200);
}

QOBS_TEST(window, specification_validation) {
  QOBS_CHECK_STATUS(validate_window_spec(spec(1000, 100, 1, 100)));
  QOBS_CHECK_FAILS(validate_window_spec(spec(0, 100, 1, 100)));
  QOBS_CHECK_FAILS(validate_window_spec(spec(1000, 0, 1, 100)));
  QOBS_CHECK_FAILS(validate_window_spec(spec(1000, 2000, 1, 100)));
  QOBS_CHECK_FAILS(validate_window_spec(spec(1000, 100, 1, 5)));
  QOBS_CHECK_FAILS(validate_window_spec(spec(1000, 100, 0, 100)));
  QOBS_CHECK_FAILS(validate_window_spec(spec(1000, 100, 11, 100)));
  WindowSpec unnamed = spec(1000, 100, 1, 100);
  unnamed.name.clear();
  QOBS_CHECK_FAILS(validate_window_spec(unnamed));
}

QOBS_TEST(window, aggregation_sums_only_established_deltas) {
  BucketAccumulator accumulator;
  QOBS_CHECK_STATUS(accumulator.configure(spec(1000, 100, 2, 100)));
  accumulator.observe(observation(0, 10));
  accumulator.observe(observation(50, 20));
  WindowObservation unknown = observation(150, 30, false);
  unknown.drops.known = false;
  unknown.any_delta_unknown = true;
  accumulator.observe(unknown);
  const WindowAggregate aggregate = accumulator.aggregate(200, FreshnessPolicy{});
  QOBS_CHECK_EQ(aggregate.sample_count, 3u);
  QOBS_CHECK_EQ(aggregate.covered_buckets, 2u);
  QOBS_CHECK_EQ(aggregate.occupancy_max.value(), 30u);
  QOBS_CHECK_EQ(aggregate.occupancy_min.value(), 10u);
  QOBS_CHECK_EQ(aggregate.occupancy_excursion, 20u);
  QOBS_CHECK_EQ(aggregate.enqueues, 2u);  // the third observation has no known delta
  QOBS_CHECK_EQ(aggregate.unknown_delta_samples, 1u);
  QOBS_CHECK(aggregate.coverage_sufficient);
  QOBS_CHECK_EQ(aggregate.assessment.quality, EvidenceQuality::Incomplete);
  QOBS_CHECK_EQ(aggregate.assessment.provenance, Provenance::Correlated);
}

QOBS_TEST(window, insufficient_coverage_is_reported_not_hidden) {
  BucketAccumulator accumulator;
  QOBS_CHECK_STATUS(accumulator.configure(spec(1000, 100, 4, 100)));
  accumulator.observe(observation(0, 10));
  const WindowAggregate aggregate = accumulator.aggregate(200, FreshnessPolicy{});
  QOBS_CHECK(!aggregate.coverage_sufficient);
  QOBS_CHECK_EQ(aggregate.assessment.quality, EvidenceQuality::Incomplete);
  QOBS_CHECK(has_flag(aggregate.assessment.flags, EvidenceFlag::CoverageInsufficient));
}

QOBS_TEST(window, empty_window_is_unknown_not_healthy) {
  BucketAccumulator accumulator;
  QOBS_CHECK_STATUS(accumulator.configure(spec(1000, 100, 1, 100)));
  const WindowAggregate aggregate = accumulator.aggregate(500, FreshnessPolicy{});
  QOBS_CHECK_EQ(aggregate.covered_buckets, 0u);
  QOBS_CHECK_EQ(aggregate.assessment.quality, EvidenceQuality::Unknown);
  QOBS_CHECK_EQ(aggregate.assessment.freshness, Freshness::Unknown);
  QOBS_CHECK(!aggregate.occupancy_max.has_value());
}

QOBS_TEST(window, expired_buckets_leave_the_window) {
  BucketAccumulator accumulator;
  QOBS_CHECK_STATUS(accumulator.configure(spec(1000, 100, 1, 100)));
  accumulator.observe(observation(0, 10));
  accumulator.expire(5000);
  QOBS_CHECK_EQ(accumulator.bucket_count(), 0u);
  QOBS_CHECK(accumulator.evicted_buckets() >= 1u);
  const WindowAggregate aggregate = accumulator.aggregate(5000, FreshnessPolicy{});
  QOBS_CHECK_EQ(aggregate.sample_count, 0u);
}

QOBS_TEST(window, late_observations_are_rejected_and_counted) {
  BucketAccumulator accumulator;
  QOBS_CHECK_STATUS(accumulator.configure(spec(1000, 100, 1, 16)));
  accumulator.observe(observation(1000, 1));
  accumulator.observe(observation(1100, 1));
  accumulator.observe(observation(1200, 1));
  accumulator.observe(observation(1300, 1));
  accumulator.observe(observation(900, 1));  // older than anything retained
  QOBS_CHECK_EQ(accumulator.late_dropped(), 1u);
}

QOBS_TEST(window, microburst_evidence_is_bounded_by_coverage) {
  BucketAccumulator accumulator;
  QOBS_CHECK_STATUS(accumulator.configure(spec(1000, 100, 3, 100)));
  accumulator.observe(observation(0, 10));
  accumulator.observe(observation(100, 900));
  accumulator.observe(observation(200, 20));
  const auto evidence = accumulator.burst_evidence(100, 3);
  QOBS_CHECK(evidence.detected);
  QOBS_CHECK(evidence.coverage_sufficient);
  QOBS_CHECK_EQ(evidence.excursion, 890u);
  QOBS_CHECK_EQ(evidence.covered_buckets, 3u);
  QOBS_CHECK(!evidence.sub_sample_reconstructed);

  BucketAccumulator sparse;
  QOBS_CHECK_STATUS(sparse.configure(spec(1000, 100, 3, 100)));
  sparse.observe(observation(0, 10));
  sparse.observe(observation(100, 900));
  const auto thin = sparse.burst_evidence(100, 3);
  QOBS_CHECK(!thin.detected);
  QOBS_CHECK(!thin.coverage_sufficient);
  QOBS_CHECK(!thin.sub_sample_reconstructed);
}

QOBS_TEST(window, saturating_sums_are_counted) {
  BucketAccumulator accumulator;
  QOBS_CHECK_STATUS(accumulator.configure(spec(1000, 100, 1, 100)));
  WindowObservation huge = observation(0, 1, true, UINT64_MAX);
  accumulator.observe(huge);
  accumulator.observe(huge);
  const WindowAggregate aggregate = accumulator.aggregate(50, FreshnessPolicy{});
  QOBS_CHECK_EQ(aggregate.enqueues, UINT64_MAX);
  QOBS_CHECK(aggregate.saturation_events >= 1u);
}
