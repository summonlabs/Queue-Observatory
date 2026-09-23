#include "support/TestHarness.hpp"

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "qobs/ingest/Wire.hpp"
#include "qobs/policy/Classification.hpp"
#include "qobs/policy/Explanation.hpp"
#include "qobs/store/History.hpp"
#include "qobs/store/Store.hpp"
#include "qobs/store/Window.hpp"

using namespace qobs;
using qobs::test::SampleBuilder;

namespace {

/// Deterministic generator. Seeded explicitly so that a failure can be
/// reproduced by rerunning the same case.
class Generator {
 public:
  explicit Generator(std::uint64_t seed) : state_(seed == 0u ? 0x9E3779B97F4A7C15ull : seed) {}

  std::uint64_t next() {
    state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t value = state_;
    value = (value ^ (value >> 30u)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27u)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31u);
  }

  std::uint64_t bounded(std::uint64_t limit) { return limit == 0u ? 0u : next() % limit; }

  bool chance(unsigned numerator, unsigned denominator) {
    return bounded(denominator) < numerator;
  }

 private:
  std::uint64_t state_{0};
};

}  // namespace

QOBS_TEST(property, counter_continuity_never_invents_progress) {
  for (std::uint64_t seed = 1; seed <= 250u; ++seed) {
    Generator generator(seed);
    CounterState state;
    ContinuityPolicy policy;
    policy.width = CounterWidth::Bits32;
    policy.max_sample_gap_ns = 1000000LL;
    std::uint64_t expected_known_total = 0;
    Nanos now = 0;
    std::uint64_t raw = generator.bounded(1000u);
    std::uint64_t previous_total = 0;
    for (int step = 0; step < 120; ++step) {
      const std::uint64_t roll = generator.bounded(100u);
      if (roll < 55u) {
        raw += generator.bounded(500u);
      } else if (roll < 70u) {
        raw = generator.bounded(400000u);  // an abrupt restart
      } else if (roll < 85u) {
        raw = 0xFFFFFFFFu - generator.bounded(10u);  // near the wrap boundary
      } else {
        raw = generator.bounded(1000000u);
      }
      now += static_cast<Nanos>(generator.bounded(200000u));
      ObservationTime observed;
      observed.ns = now;
      observed.domain = ClockDomainId(std::string("d"));
      const ReceiveTime received{SteadyTime{now}, WallTime{now}};
      const CounterUpdate update = update_counter(state, raw, observed, received, policy);
      if (update.delta_known) {
        expected_known_total += update.delta;
      }
      QOBS_CHECK(state.known_total >= previous_total);
      QOBS_CHECK_EQ(state.known_total, expected_known_total);
      QOBS_CHECK(continuity_yields_known_delta(update.continuity) == update.delta_known);
      previous_total = state.known_total;
    }
    // Every update after the first either produced a delta the runtime was
    // willing to justify or it did not; the two counters partition the
    // classified updates exactly, with no update counted twice.
    QOBS_CHECK_EQ(state.known_deltas + state.unknown_deltas, static_cast<std::uint64_t>(119));
    // Every classified update with no justified delta is counted once, and the
    // reason counters are a partition of those updates, never more than them.
    QOBS_CHECK(state.unknown_deltas >=
               state.resets + state.ambiguous + state.discontinuities);
    QOBS_CHECK(state.known_total <= UINT64_MAX);
    QOBS_CHECK_EQ(state.known_total, expected_known_total);
  }
}

QOBS_TEST(property, classification_is_a_total_function_with_stable_output) {
  const std::set<PressureState> documented{
      PressureState::Unknown,   PressureState::Idle,      PressureState::Normal,
      PressureState::Elevated,  PressureState::Pressured, PressureState::Saturated,
      PressureState::Dropping,  PressureState::Paused,    PressureState::Stale,
      PressureState::Conflicting};
  QOBS_CHECK_EQ(documented.size(), kPressureStateCount);

  const PressurePolicy& policy = default_pressure_policy();
  for (std::uint64_t seed = 1; seed <= 400u; ++seed) {
    Generator generator(seed * 7919u);
    ClassificationInput input;
    input.queue.device = DeviceId(std::string("leaf-01"));
    input.queue.port = PortId(std::string("ethernet1/1"));
    input.queue.queue = QueueId::from_raw(static_cast<std::uint32_t>(generator.bounded(8u)));
    input.assessment.freshness = static_cast<Freshness>(generator.bounded(5u));
    input.assessment.quality = static_cast<EvidenceQuality>(generator.bounded(5u));
    input.assessment.provenance = static_cast<Provenance>(generator.bounded(5u));
    input.assessment.authority = static_cast<SourceAuthority>(generator.bounded(4u));
    input.assessment.age_ns = static_cast<Nanos>(generator.bounded(100000u));
    if (generator.chance(3u, 4u)) {
      input.occupancy_cells = generator.bounded(2000u);
    }
    if (generator.chance(3u, 4u)) {
      input.dynamic_threshold_cells = 1u + generator.bounded(2000u);
    }
    if (generator.chance(1u, 3u)) {
      input.queue_depth_packets = generator.bounded(200u);
      input.max_depth_packets = 1u + generator.bounded(200u);
    }
    input.drops.known = generator.chance(1u, 2u);
    input.drops.delta = generator.bounded(10u);
    input.marks.known = generator.chance(1u, 2u);
    input.marks.delta = generator.bounded(10u);
    input.pause_frames.known = generator.chance(1u, 3u);
    input.pause_frames.delta = generator.bounded(4u);
    input.enqueues.known = generator.chance(2u, 3u);
    input.enqueues.delta = generator.bounded(20u);
    for (std::size_t index = 0; index < kSampleFieldCount; ++index) {
      if (generator.chance(1u, 2u)) {
        input.reported = input.reported | field_bit(static_cast<SampleField>(index));
      }
    }
    input.peer_conflict = generator.chance(1u, 8u);

    const ClassificationResult first = classify(policy, input);
    const ClassificationResult second = classify(policy, input);
    QOBS_CHECK(documented.count(first.state) == 1u);
    QOBS_CHECK_EQ(first.state, second.state);
    QOBS_CHECK_EQ(first.explanation_digest, second.explanation_digest);
    QOBS_CHECK_EQ(render_explanation(first), render_explanation(second));
    QOBS_CHECK(first.trace.size() <= kRuleCount);
    QOBS_CHECK(!first.trace.empty());
    QOBS_CHECK_EQ(QOBS_BACK(first.trace).rule, first.deciding_rule);
    QOBS_CHECK(QOBS_BACK(first.trace).matched);
  }
}

QOBS_TEST(property, non_fresh_evidence_never_produces_a_live_state) {
  const PressurePolicy& policy = default_pressure_policy();
  const std::set<PressureState> live{PressureState::Idle, PressureState::Normal,
                                     PressureState::Elevated, PressureState::Pressured,
                                     PressureState::Saturated, PressureState::Dropping,
                                     PressureState::Paused};
  for (std::uint64_t seed = 1; seed <= 200u; ++seed) {
    Generator generator(seed * 104729u);
    ClassificationInput input;
    input.queue.device = DeviceId(std::string("leaf-01"));
    input.queue.port = PortId(std::string("ethernet1/1"));
    input.queue.queue = QueueId::from_raw(0);
    input.assessment.freshness =
        static_cast<Freshness>(2u + generator.bounded(3u));  // Aging, Stale or Expired
    input.assessment.quality = EvidenceQuality::Complete;
    input.assessment.authority = SourceAuthority::Authoritative;
    input.assessment.provenance = Provenance::Observed;
    input.reported = field_bit(SampleField::OccupancyCells) |
                     field_bit(SampleField::DynamicThresholdCells);
    input.occupancy_cells = generator.bounded(2000u);
    input.dynamic_threshold_cells = 1000u;
    input.drops.known = true;
    input.drops.delta = generator.bounded(100u);
    const ClassificationResult result = classify(policy, input);
    QOBS_CHECK(live.count(result.state) == 0u);
  }
}

QOBS_TEST(property, window_aggregates_respect_their_own_bounds) {
  for (std::uint64_t seed = 1; seed <= 120u; ++seed) {
    Generator generator(seed * 15485863u);
    WindowSpec spec;
    spec.name = "property";
    spec.duration_ns = 1000000;
    spec.bucket_ns = 10000;
    spec.min_covered_buckets = 1;
    spec.max_buckets = 256;
    BucketAccumulator accumulator;
    QOBS_CHECK_STATUS(accumulator.configure(spec));
    const std::size_t observations = 1u + static_cast<std::size_t>(generator.bounded(400u));
    Nanos now = 0;
    for (std::size_t index = 0; index < observations; ++index) {
      WindowObservation observation;
      now += static_cast<Nanos>(generator.bounded(20000u));
      observation.steady_ns = now;
      observation.has_occupancy = generator.chance(4u, 5u);
      observation.occupancy = generator.bounded(10000u);
      observation.drops.known = generator.chance(2u, 3u);
      observation.drops.delta = generator.bounded(5u);
      observation.establishes_current = generator.chance(4u, 5u);
      accumulator.observe(observation);
    }
    const WindowAggregate aggregate = accumulator.aggregate(now, FreshnessPolicy{});
    QOBS_CHECK(aggregate.bucket_count <= 101u);  // the window never exceeds its span
    QOBS_CHECK(aggregate.covered_buckets <= aggregate.bucket_count);
    if (aggregate.occupancy_max.has_value()) {
      QOBS_CHECK(aggregate.occupancy_min.has_value());
      QOBS_CHECK(*aggregate.occupancy_max >= *aggregate.occupancy_min);
      QOBS_CHECK_EQ(aggregate.occupancy_excursion,
                    *aggregate.occupancy_max - *aggregate.occupancy_min);
    } else {
      QOBS_CHECK_EQ(aggregate.occupancy_excursion, 0u);
    }
    if (aggregate.covered_buckets < spec.min_covered_buckets) {
      QOBS_CHECK(!aggregate.coverage_sufficient);
    }
  }
}

QOBS_TEST(property, wire_round_trip_preserves_every_reported_field) {
  for (std::uint64_t seed = 1; seed <= 200u; ++seed) {
    Generator generator(seed * 32452843u);
    SampleBuilder builder("leaf-01", "ethernet1/1",
                          static_cast<std::uint32_t>(generator.bounded(16u)), "collector-a",
                          "boot-1");
    builder.sequence(1u + generator.bounded(1000000u));
    builder.generation(1u + generator.bounded(100u));
    builder.observed_at(static_cast<Nanos>(generator.bounded(1000000000u)));
    builder.received_at(static_cast<Nanos>(generator.bounded(1000000000u)));
    builder.authority(static_cast<SourceAuthority>(generator.bounded(4u)));
    for (std::size_t index = 0; index < kSampleFieldCount; ++index) {
      if (generator.chance(1u, 2u)) {
        builder.set(static_cast<SampleField>(index), generator.next());
      }
    }
    if (generator.chance(1u, 2u)) {
      builder.classes(static_cast<std::uint16_t>(generator.bounded(64u)), "sp0");
    }
    const QueueSample original = builder.build();

    std::string encoded;
    QOBS_CHECK_STATUS(encode_sample(original, encoded));
    DecodeOutcome outcome;
    QOBS_CHECK_STATUS(decode_document(encoded, IngestLimits{}, outcome));
    QOBS_CHECK_EQ(outcome.sample_count, 1u);
    QOBS_REQUIRE(outcome.samples.size() == 1u);
    const QueueSample& decoded = QOBS_FRONT(outcome.samples);
    QOBS_CHECK_EQ(decoded.queue, original.queue);
    QOBS_CHECK_EQ(decoded.source, original.source);
    QOBS_CHECK_EQ(decoded.incarnation, original.incarnation);
    QOBS_CHECK_EQ(decoded.generation.value(), original.generation.value());
    QOBS_CHECK_EQ(decoded.sequence.value(), original.sequence.value());
    QOBS_CHECK_EQ(decoded.observed.ns, original.observed.ns);
    QOBS_CHECK_EQ(decoded.authority, original.authority);
    QOBS_CHECK_EQ(decoded.reported, original.reported);
    QOBS_CHECK_EQ(decoded.declared, original.declared);
    for (std::size_t index = 0; index < kSampleFieldCount; ++index) {
      const auto field = static_cast<SampleField>(index);
      QOBS_CHECK_EQ(decoded.value(field).has_value(), original.value(field).has_value());
      if (decoded.value(field).has_value()) {
        QOBS_CHECK_EQ(*decoded.value(field), *original.value(field));
      }
    }
  }
}

QOBS_TEST(property, store_accepts_exactly_the_samples_it_admits) {
  auto clock = std::make_shared<ManualClock>(SteadyTime{0});
  HistoryLimits limits;
  limits.max_queues = 8;
  limits.max_samples_per_queue = 16;
  limits.max_events_total = 1024;
  QueueStore store(PressurePolicy{}, limits, QueryLimits{}, clock);
  Generator generator(20260217u);
  std::uint64_t accepted_expected = 0;
  std::uint64_t presented = 0;
  Nanos now = 1000;
  for (std::uint64_t round = 0; round < 400u; ++round) {
    const std::uint32_t queue_index = static_cast<std::uint32_t>(generator.bounded(3u));
    SampleBuilder builder("leaf-01", "ethernet1/1", queue_index, "collector-a", "boot-1");
    now += 1000;
    builder.sequence(round + 1u);
    builder.received_at(now);
    builder.observed_at(now);
    builder.set(SampleField::OccupancyCells, generator.bounded(2000u));
    builder.set(SampleField::DynamicThresholdCells, 1000u);
    builder.set(SampleField::EnqueuePackets, generator.bounded(100u));
    builder.set(SampleField::DropPackets, generator.bounded(4u));
    AdmissionResult result;
    const Status status = store.ingest(builder.build(), result);
    ++presented;
    if (status.ok() && result.accepted) {
      ++accepted_expected;
    }
    QOBS_CHECK_EQ(store.counters().samples_accepted, accepted_expected);
    QOBS_CHECK_EQ(store.counters().samples_presented, presented);
  }
  QOBS_CHECK(accepted_expected > 300u);
  const StoreCounters counters = store.counters();
  QOBS_CHECK_EQ(counters.samples_accepted + counters.samples_rejected + counters.samples_fenced,
                presented);
}

QOBS_TEST(property, history_rings_never_exceed_their_capacity) {
  for (std::uint64_t seed = 1; seed <= 60u; ++seed) {
    Generator generator(seed * 49979687u);
    const std::size_t capacity = 1u + static_cast<std::size_t>(generator.bounded(32u));
    HistoryRing ring;
    ring.reserve(capacity);
    const std::size_t pushes = static_cast<std::size_t>(generator.bounded(200u));
    for (std::size_t index = 0; index < pushes; ++index) {
      HistoryRecord record;
      record.received.steady = SteadyTime{static_cast<Nanos>(index)};
      ring.push(record);
    }
    QOBS_CHECK(ring.size() <= capacity);
    QOBS_CHECK_EQ(ring.size(), pushes < capacity ? pushes : capacity);
    QOBS_CHECK_EQ(ring.evicted(), pushes < capacity ? 0u : static_cast<std::uint64_t>(pushes - capacity));
    const std::vector<HistoryRecord> snapshot = ring.snapshot();
    QOBS_CHECK_EQ(snapshot.size(), ring.size());
    for (std::size_t index = 1; index < snapshot.size(); ++index) {
      QOBS_CHECK(snapshot[index].received.steady.ns >= snapshot[index - 1u].received.steady.ns);
    }
  }
}
