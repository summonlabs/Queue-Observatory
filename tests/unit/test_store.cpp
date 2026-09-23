#include "support/TestHarness.hpp"

#include <memory>

#include "qobs/store/Store.hpp"

using namespace qobs;
using qobs::test::SampleBuilder;

namespace {

struct Fixture {
  std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>(SteadyTime{1000});
  PressurePolicy policy{};
  HistoryLimits history{};
  std::unique_ptr<QueueStore> store{};

  Fixture() {
    history.max_queues = 4;
    history.max_samples_per_queue = 8;
    history.max_events_per_queue = 32;
    history.max_events_total = 256;
    store = std::make_unique<QueueStore>(policy, history, QueryLimits{}, clock);
  }

  AdmissionResult push(const QueueSample& sample) {
    AdmissionResult result;
    const Status status = store->ingest(sample, result);
    (void)status;
    return result;
  }
};

SampleBuilder base(std::uint64_t sequence) {
  SampleBuilder builder("leaf-01", "ethernet1/1", 0u, "collector-a", "boot-1");
  builder.sequence(sequence).generation(1).received_at(1000).observed_at(1000);
  builder.set(SampleField::OccupancyCells, 0);
  builder.set(SampleField::DynamicThresholdCells, 1000);
  builder.set(SampleField::EnqueuePackets, 0);
  return builder;
}

}  // namespace

QOBS_TEST(store, accepts_a_first_sample_and_reports_a_state) {
  Fixture fixture;
  const AdmissionResult result = fixture.push(base(1).build());
  QOBS_CHECK(result.accepted);
  QOBS_CHECK_EQ(result.outcome, AdmissionOutcome::AcceptedNewSource);
  QOBS_CHECK(result.queue_created);
  // The first observation of a counter only establishes its baseline, so the
  // enqueue delta is not yet known and idleness is not claimed.
  QOBS_CHECK_EQ(result.classification.state, PressureState::Normal);
  QOBS_CHECK_EQ(fixture.store->queue_count(), 1u);

  SampleBuilder second = base(2);
  second.received_at(1100).observed_at(1100);
  const AdmissionResult settled = fixture.push(second.build());
  QOBS_CHECK_EQ(settled.classification.state, PressureState::Idle);
}

QOBS_TEST(store, duplicate_and_regressed_sequences_are_fenced) {
  Fixture fixture;
  (void)fixture.push(base(5).build());
  const AdmissionResult duplicate = fixture.push(base(5).build());
  QOBS_CHECK(!duplicate.accepted);
  QOBS_CHECK_EQ(duplicate.outcome, AdmissionOutcome::FencedStaleSequence);
  const AdmissionResult regressed = fixture.push(base(4).set(SampleField::OccupancyCells, 1u).build());
  QOBS_CHECK(!regressed.accepted);
  QOBS_CHECK_EQ(regressed.outcome, AdmissionOutcome::FencedStaleSequence);
  const AdmissionResult next = fixture.push(base(6).set(SampleField::OccupancyCells, 1u).build());
  QOBS_CHECK(next.accepted);
  QOBS_CHECK_EQ(fixture.store->counters().duplicates_suppressed, 2u);
}

QOBS_TEST(store, a_new_incarnation_resets_the_sequence_space) {
  Fixture fixture;
  (void)fixture.push(base(100).build());
  SampleBuilder restarted("leaf-01", "ethernet1/1", 0u, "collector-a", "boot-2");
  restarted.sequence(1).generation(1).received_at(2000).observed_at(2000);
  restarted.set(SampleField::OccupancyCells, 5u);
  const AdmissionResult result = fixture.push(restarted.build());
  QOBS_CHECK(result.accepted);
  QOBS_CHECK_EQ(result.outcome, AdmissionOutcome::AcceptedNewIncarnation);
}

QOBS_TEST(store, a_superseded_incarnation_is_fenced) {
  Fixture fixture;
  (void)fixture.push(base(1).build());
  SampleBuilder second("leaf-01", "ethernet1/1", 0u, "collector-a", "boot-2");
  second.sequence(1).received_at(2000).observed_at(2000);
  second.set(SampleField::OccupancyCells, 5u);
  (void)fixture.push(second.build());
  const AdmissionResult replay = fixture.push(base(2).build());
  QOBS_CHECK(!replay.accepted);
  QOBS_CHECK_EQ(replay.outcome, AdmissionOutcome::FencedStaleIncarnation);
}

QOBS_TEST(store, generation_and_authority_regressions_are_fenced) {
  Fixture fixture;
  (void)fixture.push(base(1).generation(5).build());
  const AdmissionResult stale = fixture.push(base(2).generation(4).build());
  QOBS_CHECK_EQ(stale.outcome, AdmissionOutcome::FencedStaleGeneration);

  const AdmissionResult raised = fixture.push(base(3).generation(6).authority(
      SourceAuthority::Authoritative).build());
  QOBS_CHECK_EQ(raised.outcome, AdmissionOutcome::AcceptedNewGeneration);

  const AdmissionResult downgrade =
      fixture.push(base(4).generation(6).authority(SourceAuthority::Primary).build());
  QOBS_CHECK_EQ(downgrade.outcome, AdmissionOutcome::FencedAuthorityDowngrade);
}

QOBS_TEST(store, structurally_invalid_samples_are_rejected) {
  Fixture fixture;
  QueueSample empty;
  AdmissionResult result;
  QOBS_CHECK_FAILS(fixture.store->ingest(empty, result));
  QOBS_CHECK_EQ(result.outcome, AdmissionOutcome::RejectedInvalid);

  QueueSample no_fields = base(1).build();
  no_fields.reported = 0;
  QOBS_CHECK_FAILS(fixture.store->ingest(no_fields, result));
  QOBS_CHECK_EQ(result.outcome, AdmissionOutcome::RejectedInvalid);

  QueueSample undeclared = base(1).build();
  undeclared.declared = field_bit(SampleField::OccupancyCells);
  QOBS_CHECK_FAILS(fixture.store->ingest(undeclared, result));
  QOBS_CHECK_EQ(result.outcome, AdmissionOutcome::RejectedInvalid);
}

QOBS_TEST(store, a_reported_field_is_never_confused_with_a_missing_one) {
  Fixture fixture;
  QueueSample sparse = base(1).build();
  sparse.clear_value(SampleField::OccupancyCells);
  sparse.clear_value(SampleField::DynamicThresholdCells);
  sparse.declared = sparse.reported;
  const AdmissionResult result = fixture.push(sparse);
  QOBS_CHECK(result.accepted);
  QOBS_CHECK_EQ(result.classification.state, PressureState::Unknown);
  QOBS_CHECK_EQ(result.classification.deciding_rule, RuleId::RequiredFields);
}

QOBS_TEST(store, queue_and_history_budgets_are_enforced) {
  Fixture fixture;
  for (std::uint32_t index = 0; index < 4u; ++index) {
    SampleBuilder builder("leaf-01", "ethernet1/1", index, "collector-a", "boot-1");
    builder.sequence(index + 1u).received_at(1000).observed_at(1000);
    builder.set(SampleField::OccupancyCells, 1u);
    (void)fixture.push(builder.build());
  }
  QOBS_CHECK_EQ(fixture.store->queue_count(), 4u);
  SampleBuilder overflow("leaf-01", "ethernet1/1", 99u, "collector-a", "boot-1");
  overflow.sequence(5).received_at(1000).observed_at(1000);
  overflow.set(SampleField::OccupancyCells, 1u);
  AdmissionResult result;
  QOBS_CHECK_FAILS(fixture.store->ingest(overflow.build(), result));
  QOBS_CHECK_EQ(result.outcome, AdmissionOutcome::RejectedLimit);
}

QOBS_TEST(store, equal_authority_sources_conflict_and_then_agree) {
  Fixture fixture;
  SampleBuilder first("leaf-01", "ethernet1/1", 0u, "collector-a", "boot-a");
  first.sequence(1).received_at(1000).observed_at(1000).authority(SourceAuthority::Primary);
  first.set(SampleField::OccupancyCells, 100u);
  first.set(SampleField::DynamicThresholdCells, 1000u);
  const AdmissionResult accepted = fixture.push(first.build());
  QOBS_CHECK(accepted.accepted);
  QOBS_CHECK(!accepted.conflict_detected);

  SampleBuilder second("leaf-01", "ethernet1/1", 0u, "collector-b", "boot-b");
  second.sequence(1).received_at(1100).observed_at(1100).authority(SourceAuthority::Primary);
  second.set(SampleField::OccupancyCells, 900u);
  second.set(SampleField::DynamicThresholdCells, 1000u);
  const AdmissionResult conflicting = fixture.push(second.build());
  QOBS_CHECK(conflicting.accepted);
  QOBS_CHECK(conflicting.conflict_detected);
  QOBS_CHECK_EQ(conflicting.classification.state, PressureState::Conflicting);

  SampleBuilder agreed("leaf-01", "ethernet1/1", 0u, "collector-b", "boot-b");
  agreed.sequence(2).received_at(1200).observed_at(1200).authority(SourceAuthority::Primary);
  agreed.set(SampleField::OccupancyCells, 110u);
  agreed.set(SampleField::DynamicThresholdCells, 1000u);
  const AdmissionResult settled = fixture.push(agreed.build());
  QOBS_CHECK(!settled.conflict_detected);
  QOBS_CHECK_NE(settled.classification.state, PressureState::Conflicting);
  QOBS_CHECK_EQ(fixture.store->counters().conflicts_detected, 1u);
  QOBS_CHECK_EQ(fixture.store->counters().conflicts_cleared, 1u);
}

QOBS_TEST(store, different_authorities_do_not_conflict) {
  Fixture fixture;
  SampleBuilder primary("leaf-01", "ethernet1/1", 0u, "collector-a", "boot-a");
  primary.sequence(1).received_at(1000).observed_at(1000).authority(SourceAuthority::Primary);
  primary.set(SampleField::OccupancyCells, 100u);
  primary.set(SampleField::DynamicThresholdCells, 1000u);
  (void)fixture.push(primary.build());

  SampleBuilder secondary("leaf-01", "ethernet1/1", 0u, "collector-b", "boot-b");
  secondary.sequence(1).received_at(1100).observed_at(1100).authority(SourceAuthority::Secondary);
  secondary.set(SampleField::OccupancyCells, 900u);
  secondary.set(SampleField::DynamicThresholdCells, 1000u);
  const AdmissionResult result = fixture.push(secondary.build());
  QOBS_CHECK(result.accepted);
  QOBS_CHECK(!result.conflict_detected);
  QOBS_CHECK_NE(result.classification.state, PressureState::Conflicting);
}

QOBS_TEST(store, counter_wrap_and_reset_are_recorded_as_events) {
  Fixture fixture;
  SampleBuilder builder = base(1);
  builder.set(SampleField::DropPackets, 0xFFFFFFFEu);
  (void)fixture.push(builder.build());

  SampleBuilder wrapped = base(2);
  wrapped.received_at(1100).observed_at(1100);
  wrapped.set(SampleField::DropPackets, 3u);
  const AdmissionResult result = fixture.push(wrapped.build());
  QOBS_CHECK(result.accepted);
  QOBS_CHECK_EQ(fixture.store->counters().counter_wraps, 1u);
  QOBS_CHECK_EQ(result.classification.state, PressureState::Dropping);

  SampleBuilder reset = base(3);
  reset.received_at(1200).observed_at(1200);
  reset.set(SampleField::DropPackets, 1u);
  (void)fixture.push(reset.build());
  QOBS_CHECK_EQ(fixture.store->counters().counter_resets, 1u);
}

QOBS_TEST(store, queries_are_paged_and_report_truncation) {
  Fixture fixture;
  for (std::uint32_t index = 0; index < 4u; ++index) {
    SampleBuilder builder("leaf-01", "ethernet1/1", index, "collector-a", "boot-1");
    // One source, so the sequence must be strictly increasing across queues.
    builder.sequence(index + 1u).received_at(1000).observed_at(1000);
    builder.set(SampleField::OccupancyCells, 1u);
    (void)fixture.push(builder.build());
  }
  InspectQuery query;
  query.page.limit = 2;
  InspectResult result;
  QOBS_CHECK_STATUS(fixture.store->inspect(query, result));
  QOBS_CHECK_EQ(result.total_matched, 4u);
  QOBS_CHECK_EQ(result.rows.size(), 2u);
  QOBS_CHECK(result.truncated);

  query.page.offset = 2;
  QOBS_CHECK_STATUS(fixture.store->inspect(query, result));
  QOBS_CHECK_EQ(result.rows.size(), 2u);
  QOBS_CHECK(!result.truncated);
  QOBS_CHECK_EQ(QOBS_FRONT(result.rows).queue.queue.value(), 2u);
}

QOBS_TEST(store, filter_selects_by_state_and_identity) {
  Fixture fixture;
  SampleBuilder idle = base(1);
  (void)fixture.push(idle.build());
  // The first observation of a counter only establishes its baseline, so a
  // drop delta requires a baseline sample followed by one that advanced.
  SampleBuilder baseline = base(2);
  baseline.received_at(1100).observed_at(1100);
  baseline.set(SampleField::DropPackets, 0u);
  (void)fixture.push(baseline.build());
  SampleBuilder dropped = base(3);
  dropped.received_at(1200).observed_at(1200);
  dropped.set(SampleField::DropPackets, 50u);
  const AdmissionResult drop_admission = fixture.push(dropped.build());
  QOBS_CHECK_EQ(drop_admission.classification.state, PressureState::Dropping);

  PressureQuery query;
  query.filter.state = PressureState::Dropping;
  PressureResult result;
  QOBS_CHECK_STATUS(fixture.store->pressure(query, result));
  QOBS_CHECK_EQ(result.total_matched, 1u);
  QOBS_CHECK_EQ(QOBS_FRONT(result.rows).queue.queue.value(), 0u);

  query.filter.state.reset();
  query.filter.device = DeviceId(std::string("leaf-99"));
  QOBS_CHECK_STATUS(fixture.store->pressure(query, result));
  QOBS_CHECK_EQ(result.total_matched, 0u);
}

QOBS_TEST(store, explain_returns_the_recorded_decision_and_can_re_derive_it) {
  Fixture fixture;
  SampleBuilder builder = base(1);
  builder.set(SampleField::OccupancyCells, 800u);
  const AdmissionResult accepted = fixture.push(builder.build());
  QOBS_REQUIRE(accepted.accepted);

  ExplainQuery query;
  query.queue = accepted.queue;
  ExplainResult result;
  QOBS_CHECK_STATUS(fixture.store->explain(query, result));
  QOBS_CHECK(result.found);
  QOBS_CHECK_EQ(result.classification.state, PressureState::Pressured);
  QOBS_CHECK(!result.policy_overridden);
  QOBS_CHECK(result.explanation.find("state: pressured") != std::string::npos);

  PressurePolicy alternative = fixture.policy;
  alternative.relative.pressured_permille = 900;
  alternative.relative.elevated_permille = 100;
  query.policy = alternative;
  QOBS_CHECK_STATUS(fixture.store->explain(query, result));
  QOBS_CHECK(result.policy_overridden);
  QOBS_CHECK_EQ(result.classification.state, PressureState::Elevated);
}

QOBS_TEST(store, unresolved_metadata_is_reported_without_hiding_evidence) {
  Fixture fixture;
  ClassMetadataTable table;
  table.revision = Revision::from_raw(1);
  table.generation = GenerationId::from_raw(1);
  table.source = SourceId(std::string("cmdb"));
  table.authority = SourceAuthority::Authoritative;
  SchedulingClassDescriptor scheduling;
  scheduling.id = SchedulingClassId(std::string("sp0"));
  scheduling.mode = "strict-priority";
  table.scheduling_classes.push_back(scheduling);
  TrafficClassDescriptor traffic;
  traffic.id = TrafficClassId::from_raw(3);
  traffic.name = "lossless";
  traffic.scheduling_class = SchedulingClassId(std::string("sp0"));
  table.traffic_classes.push_back(traffic);

  MetadataAdmission admission;
  QOBS_CHECK_STATUS(fixture.store->apply_metadata(table, admission));
  QOBS_CHECK(admission.accepted);

  SampleBuilder builder = base(1);
  builder.classes(static_cast<std::uint16_t>(3u), std::nullopt);
  const AdmissionResult result = fixture.push(builder.build());
  QOBS_CHECK(result.accepted);

  InspectQuery query;
  InspectResult inspected;
  QOBS_CHECK_STATUS(fixture.store->inspect(query, inspected));
  QOBS_REQUIRE(inspected.rows.size() == 1u);
  QOBS_CHECK(QOBS_FRONT(inspected.rows).classes.scheduling_class.has_value());
  QOBS_CHECK_EQ(QOBS_FRONT(inspected.rows).classes.origin, AttributeOrigin::ResolvedFromMetadata);
  QOBS_CHECK(!QOBS_FRONT(inspected.rows).class_reference_unsupported);
}

QOBS_TEST(store, metadata_revisions_are_fenced) {
  Fixture fixture;
  ClassMetadataTable table;
  table.revision = Revision::from_raw(5);
  table.generation = GenerationId::from_raw(1);
  table.source = SourceId(std::string("cmdb"));
  MetadataAdmission admission;
  QOBS_CHECK_STATUS(fixture.store->apply_metadata(table, admission));
  QOBS_CHECK(admission.accepted);

  table.revision = Revision::from_raw(4);
  QOBS_CHECK_STATUS(fixture.store->apply_metadata(table, admission));
  QOBS_CHECK(admission.fenced);
  QOBS_CHECK_EQ(fixture.store->counters().metadata_fenced, 1u);

  table.revision = Revision::from_raw(6);
  QOBS_CHECK_STATUS(fixture.store->apply_metadata(table, admission));
  QOBS_CHECK(admission.accepted);
}
