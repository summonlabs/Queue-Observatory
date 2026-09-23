#include "support/TestHarness.hpp"

#include "qobs/policy/Classification.hpp"
#include "qobs/policy/Explanation.hpp"
#include "qobs/policy/Policy.hpp"

using namespace qobs;

namespace {

ClassificationInput base_input() {
  ClassificationInput input;
  input.queue.device = DeviceId(std::string("leaf-01"));
  input.queue.port = PortId(std::string("ethernet1/1"));
  input.queue.queue = QueueId::from_raw(0);
  input.assessment.freshness = Freshness::Fresh;
  input.assessment.quality = EvidenceQuality::Complete;
  input.assessment.provenance = Provenance::Observed;
  input.assessment.authority = SourceAuthority::Primary;
  input.assessment.age_ns = 0;
  input.occupancy_cells = 0;
  input.dynamic_threshold_cells = 1000;
  input.reported = field_bit(SampleField::OccupancyCells) |
                   field_bit(SampleField::DynamicThresholdCells);
  input.enqueues.known = true;
  input.enqueues.delta = 0;
  return input;
}

}  // namespace

QOBS_TEST(classification, every_documented_state_is_reachable) {
  const PressurePolicy& policy = default_pressure_policy();

  {
    ClassificationInput input = base_input();
    input.assessment.quality = EvidenceQuality::Unknown;
    input.reported = 0;
    QOBS_CHECK_EQ(classify(policy, input).state, PressureState::Unknown);
  }
  {
    ClassificationInput input = base_input();
    input.assessment.quality = EvidenceQuality::Conflicting;
    QOBS_CHECK_EQ(classify(policy, input).state, PressureState::Conflicting);
  }
  {
    ClassificationInput input = base_input();
    input.assessment.freshness = Freshness::Stale;
    QOBS_CHECK_EQ(classify(policy, input).state, PressureState::Stale);
  }
  {
    ClassificationInput input = base_input();
    input.assessment.quality = EvidenceQuality::Unsupported;
    QOBS_CHECK_EQ(classify(policy, input).state, PressureState::Unknown);
  }
  {
    ClassificationInput input = base_input();
    input.reported = field_bit(SampleField::QueueDepthPackets);
    QOBS_CHECK_EQ(classify(policy, input).state, PressureState::Unknown);
  }
  {
    ClassificationInput input = base_input();
    input.drops.known = true;
    input.drops.delta = 1;
    QOBS_CHECK_EQ(classify(policy, input).state, PressureState::Dropping);
  }
  {
    ClassificationInput input = base_input();
    input.pause_frames.known = true;
    input.pause_frames.delta = 1;
    QOBS_CHECK_EQ(classify(policy, input).state, PressureState::Paused);
  }
  {
    ClassificationInput input = base_input();
    input.occupancy_cells = 960;
    QOBS_CHECK_EQ(classify(policy, input).state, PressureState::Saturated);
  }
  {
    ClassificationInput input = base_input();
    input.occupancy_cells = 800;
    QOBS_CHECK_EQ(classify(policy, input).state, PressureState::Pressured);
  }
  {
    ClassificationInput input = base_input();
    input.occupancy_cells = 500;
    QOBS_CHECK_EQ(classify(policy, input).state, PressureState::Elevated);
  }
  {
    ClassificationInput input = base_input();
    QOBS_CHECK_EQ(classify(policy, input).state, PressureState::Idle);
  }
  {
    ClassificationInput input = base_input();
    input.occupancy_cells = 10;
    input.enqueues.delta = 5;
    QOBS_CHECK_EQ(classify(policy, input).state, PressureState::Normal);
  }
}

QOBS_TEST(classification, rule_order_is_fixed_and_explained) {
  const PressurePolicy& policy = default_pressure_policy();

  // Dropping outranks saturation, pause outranks saturation, and stale
  // outranks every level rule.
  ClassificationInput input = base_input();
  input.occupancy_cells = 1000;
  input.drops.known = true;
  input.drops.delta = 3;
  input.pause_frames.known = true;
  input.pause_frames.delta = 3;
  QOBS_CHECK_EQ(classify(policy, input).state, PressureState::Dropping);

  input.drops = CounterDelta{};
  QOBS_CHECK_EQ(classify(policy, input).state, PressureState::Paused);

  input.pause_frames = CounterDelta{};
  input.assessment.freshness = Freshness::Aging;
  ClassificationResult result = classify(policy, input);
  QOBS_CHECK_EQ(result.state, PressureState::Stale);
  QOBS_CHECK_EQ(result.deciding_rule, RuleId::EvidenceFreshness);

  input.assessment.freshness = Freshness::Fresh;
  input.assessment.quality = EvidenceQuality::Conflicting;
  result = classify(policy, input);
  QOBS_CHECK_EQ(result.state, PressureState::Conflicting);
  QOBS_CHECK_EQ(result.deciding_rule, RuleId::EvidenceConflict);
}

QOBS_TEST(classification, missing_required_field_is_never_zero) {
  const PressurePolicy& policy = default_pressure_policy();
  ClassificationInput input = base_input();
  input.reported = field_bit(SampleField::DynamicThresholdCells);
  input.occupancy_cells.reset();
  const ClassificationResult result = classify(policy, input);
  QOBS_CHECK_EQ(result.state, PressureState::Unknown);
  QOBS_CHECK_EQ(result.deciding_rule, RuleId::RequiredFields);
  QOBS_CHECK(QOBS_BACK(result.trace).detail.find("occupancy_cells") != std::string::npos);
}

QOBS_TEST(classification, unknown_delta_is_not_used_as_a_zero) {
  const PressurePolicy& policy = default_pressure_policy();
  ClassificationInput input = base_input();
  input.drops.known = false;
  input.drops.delta = 0;
  const ClassificationResult result = classify(policy, input);
  QOBS_CHECK_EQ(result.state, PressureState::Idle);
  bool saw_note = false;
  for (const TraceEntry& entry : result.trace) {
    if (entry.rule == RuleId::DroppingEvidence && !entry.matched &&
        entry.detail.find("unknown") != std::string::npos) {
      saw_note = true;
    }
  }
  QOBS_CHECK(saw_note);
}

QOBS_TEST(classification, absolute_basis_is_used_when_no_limit_is_reported) {
  PressurePolicy policy;
  policy.absolute.elevated_at = 10u;
  policy.absolute.pressured_at = 20u;
  policy.absolute.saturated_at = 30u;
  QOBS_CHECK_STATUS(validate_policy(policy));

  ClassificationInput input = base_input();
  input.dynamic_threshold_cells.reset();
  input.reported = field_bit(SampleField::OccupancyCells);
  input.occupancy_cells = 25;
  const ClassificationResult result = classify(policy, input);
  QOBS_CHECK_EQ(result.basis.kind, PressureBasisKind::Absolute);
  QOBS_CHECK_EQ(result.state, PressureState::Pressured);
}

QOBS_TEST(classification, packet_depth_basis_when_cells_are_absent) {
  const PressurePolicy& policy = default_pressure_policy();
  ClassificationInput input = base_input();
  input.occupancy_cells.reset();
  input.dynamic_threshold_cells.reset();
  input.queue_depth_packets = 90;
  input.max_depth_packets = 100;
  input.reported = field_bit(SampleField::QueueDepthPackets) |
                   field_bit(SampleField::MaxDepthPackets);
  const ClassificationResult result = classify(policy, input);
  QOBS_CHECK_EQ(result.basis.kind, PressureBasisKind::PacketsAgainstDepth);
  QOBS_CHECK_EQ(result.state, PressureState::Pressured);
}

QOBS_TEST(classification, congestion_marks_raise_pressure_without_occupancy) {
  const PressurePolicy& policy = default_pressure_policy();
  ClassificationInput input = base_input();
  input.occupancy_cells = 1;
  input.marks.known = true;
  input.marks.delta = 1;
  const ClassificationResult result = classify(policy, input);
  QOBS_CHECK_EQ(result.state, PressureState::Pressured);
  QOBS_CHECK_EQ(result.deciding_rule, RuleId::PressureLevel);
}

QOBS_TEST(classification, decisions_are_reproducible) {
  const PressurePolicy& policy = default_pressure_policy();
  ClassificationInput input = base_input();
  input.occupancy_cells = 800;
  const ClassificationResult first = classify(policy, input);
  for (int iteration = 0; iteration < 8; ++iteration) {
    const ClassificationResult again = classify(policy, input);
    QOBS_CHECK_EQ(again.state, first.state);
    QOBS_CHECK_EQ(again.explanation_digest, first.explanation_digest);
    QOBS_CHECK_EQ(render_explanation(again), render_explanation(first));
    QOBS_CHECK_EQ(render_explanation_summary(again), render_explanation_summary(first));
  }
  QOBS_CHECK_EQ(explanation_digest(first), explanation_digest(first));
}

QOBS_TEST(classification, digest_changes_with_the_policy) {
  PressurePolicy policy = default_pressure_policy();
  ClassificationInput input = base_input();
  input.occupancy_cells = 800;
  const ClassificationResult baseline = classify(policy, input);

  policy.relative.pressured_permille = 900;
  const ClassificationResult changed = classify(policy, input);
  QOBS_CHECK_NE(changed.explanation_digest, baseline.explanation_digest);

  policy = default_pressure_policy();
  policy.version = 99;
  const ClassificationResult versioned = classify(policy, input);
  QOBS_CHECK_NE(versioned.explanation_digest, baseline.explanation_digest);
}

QOBS_TEST(classification, explanation_lines_are_ordered_and_complete) {
  const PressurePolicy& policy = default_pressure_policy();
  ClassificationInput input = base_input();
  input.occupancy_cells = 10;
  const ClassificationResult result = classify(policy, input);
  const std::vector<ExplanationLine> lines = explanation_lines(result);
  QOBS_REQUIRE(lines.size() >= 10u);
  QOBS_CHECK_EQ(lines[0].key, std::string("policy"));
  QOBS_CHECK_EQ(lines[1].key, std::string("queue"));
  QOBS_CHECK_EQ(lines[2].key, std::string("state"));
  QOBS_CHECK_EQ(lines[1].value, std::string("leaf-01|ethernet1/1|0"));
  // Reaching the default rule means every rule was evaluated exactly once.
  QOBS_CHECK_EQ(result.trace.size(), kRuleCount);
  QOBS_CHECK_EQ(QOBS_FRONT(result.trace).rule, RuleId::EvidencePresence);
  QOBS_CHECK_EQ(QOBS_BACK(result.trace).rule, RuleId::DefaultNormal);
}

QOBS_TEST(classification, peer_conflict_flag_forces_conflicting_state) {
  const PressurePolicy& policy = default_pressure_policy();
  ClassificationInput input = base_input();
  input.peer_conflict = true;
  input.assessment.quality = EvidenceQuality::Complete;
  QOBS_CHECK_EQ(classify(policy, input).state, PressureState::Conflicting);
}

QOBS_TEST(classification, idle_requires_a_known_zero_enqueue_delta) {
  const PressurePolicy& policy = default_pressure_policy();
  ClassificationInput input = base_input();
  input.enqueues.known = false;
  const ClassificationResult result = classify(policy, input);
  QOBS_CHECK_EQ(result.state, PressureState::Normal);
  bool saw_note = false;
  for (const TraceEntry& entry : result.trace) {
    if (entry.rule == RuleId::IdleEvidence && !entry.matched &&
        entry.detail.find("idleness is not claimed") != std::string::npos) {
      saw_note = true;
    }
  }
  QOBS_CHECK(saw_note);
}
