#include "qobs/policy/Classification.hpp"

#include <algorithm>

#include "qobs/core/Text.hpp"
#include "qobs/policy/Explanation.hpp"

namespace qobs {
namespace {

std::string field_list(FieldMask mask) {
  std::string out;
  for (std::size_t index = 0; index < kSampleFieldCount; ++index) {
    const auto field = static_cast<SampleField>(index);
    if (!has_field(mask, field)) {
      continue;
    }
    if (!out.empty()) {
      out.push_back(',');
    }
    out.append(to_string(field));
  }
  if (out.empty()) {
    out.append("none");
  }
  return out;
}

std::string unsigned_text(std::uint64_t value) { return std::to_string(value); }

}  // namespace

std::string_view rule_name(RuleId id) noexcept {
  switch (id) {
    case RuleId::EvidencePresence:
      return "evidence_presence";
    case RuleId::EvidenceConflict:
      return "evidence_conflict";
    case RuleId::EvidenceUnsupported:
      return "evidence_unsupported";
    case RuleId::EvidenceFreshness:
      return "evidence_freshness";
    case RuleId::RequiredFields:
      return "required_fields";
    case RuleId::PressureBasis:
      return "pressure_basis";
    case RuleId::DroppingEvidence:
      return "dropping_evidence";
    case RuleId::PauseEvidence:
      return "pause_evidence";
    case RuleId::SaturationLevel:
      return "saturation_level";
    case RuleId::PressureLevel:
      return "pressure_level";
    case RuleId::ElevationLevel:
      return "elevation_level";
    case RuleId::IdleEvidence:
      return "idle_evidence";
    case RuleId::DefaultNormal:
      return "default_normal";
  }
  return "unmapped";
}

PressureBasis resolve_pressure_basis(const PressurePolicy& policy, const ClassificationInput& input) {
  PressureBasis basis;

  // A measurement only counts when the sample actually reported the field it
  // belongs to. A value without its reported bit is a caller error, and
  // treating it as evidence would make an unreported field behave like a
  // reported one.
  const auto usable = [&input](SampleField field,
                               const std::optional<std::uint64_t>& value) -> bool {
    return value.has_value() && has_field(input.reported, field);
  };

  const auto try_cells = [&]() -> bool {
    if (!usable(SampleField::OccupancyCells, input.occupancy_cells)) {
      return false;
    }
    if (usable(SampleField::DynamicThresholdCells, input.dynamic_threshold_cells)) {
      basis.kind = PressureBasisKind::CellsAgainstThreshold;
      basis.value_field = SampleField::OccupancyCells;
      basis.value = input.occupancy_cells;
      basis.limit_field = SampleField::DynamicThresholdCells;
      basis.limit = input.dynamic_threshold_cells;
      basis.label = "cells:dynamic_threshold";
      return true;
    }
    if (usable(SampleField::StaticThresholdCells, input.static_threshold_cells)) {
      basis.kind = PressureBasisKind::CellsAgainstThreshold;
      basis.value_field = SampleField::OccupancyCells;
      basis.value = input.occupancy_cells;
      basis.limit_field = SampleField::StaticThresholdCells;
      basis.limit = input.static_threshold_cells;
      basis.label = "cells:static_threshold";
      return true;
    }
    return false;
  };

  const auto try_packets = [&]() -> bool {
    if (usable(SampleField::QueueDepthPackets, input.queue_depth_packets) &&
        usable(SampleField::MaxDepthPackets, input.max_depth_packets)) {
      basis.kind = PressureBasisKind::PacketsAgainstDepth;
      basis.value_field = SampleField::QueueDepthPackets;
      basis.value = input.queue_depth_packets;
      basis.limit_field = SampleField::MaxDepthPackets;
      basis.limit = input.max_depth_packets;
      basis.label = "packets:max_depth";
      return true;
    }
    return false;
  };

  if (policy.prefer_cells_over_packets) {
    if (try_cells() || try_packets()) {
      return basis;
    }
  } else if (try_packets() || try_cells()) {
    return basis;
  }

  if (policy.allow_absolute_thresholds) {
    const bool have_absolute = policy.absolute.elevated_at.has_value() ||
                               policy.absolute.pressured_at.has_value() ||
                               policy.absolute.saturated_at.has_value();
    if (have_absolute && usable(SampleField::OccupancyCells, input.occupancy_cells)) {
      basis.kind = PressureBasisKind::Absolute;
      basis.value_field = SampleField::OccupancyCells;
      basis.value = input.occupancy_cells;
      basis.label = "cells:absolute";
      return basis;
    }
    if (have_absolute && policy.allow_byte_basis &&
        usable(SampleField::OccupancyBytes, input.occupancy_bytes)) {
      basis.kind = PressureBasisKind::Absolute;
      basis.value_field = SampleField::OccupancyBytes;
      basis.value = input.occupancy_bytes;
      basis.label = "bytes:absolute";
      return basis;
    }
  }

  // Record which measurement was present even though no limit was available, so
  // the explanation can say precisely what was missing.
  if (usable(SampleField::OccupancyCells, input.occupancy_cells)) {
    basis.value_field = SampleField::OccupancyCells;
    basis.value = input.occupancy_cells;
    basis.label = "cells:no_threshold_reported";
  } else if (usable(SampleField::QueueDepthPackets, input.queue_depth_packets)) {
    basis.value_field = SampleField::QueueDepthPackets;
    basis.value = input.queue_depth_packets;
    basis.label = "packets:no_depth_limit_reported";
  } else if (usable(SampleField::OccupancyBytes, input.occupancy_bytes)) {
    basis.value_field = SampleField::OccupancyBytes;
    basis.value = input.occupancy_bytes;
    basis.label = "bytes:no_threshold_reported";
  } else {
    basis.label = "no_measurement_reported";
  }
  basis.kind = PressureBasisKind::None;
  return basis;
}

ClassificationResult classify(const PressurePolicy& policy, const ClassificationInput& input) {
  ClassificationResult result;
  result.queue_text = input.queue.to_string();
  result.policy_version = policy.version;
  result.policy_name = policy.name;
  result.assessment = input.assessment;
  result.trace.reserve(kRuleCount);

  const auto record = [&result](RuleId rule, bool matched, std::string detail,
                                std::uint64_t operand = 0, std::uint64_t threshold = 0) {
    TraceEntry entry;
    entry.rule = rule;
    entry.matched = matched;
    entry.detail = std::move(detail);
    entry.operand = operand;
    entry.threshold = threshold;
    result.trace.push_back(std::move(entry));
  };

  const auto conclude = [&result](PressureState state, RuleId rule) {
    result.state = state;
    result.deciding_rule = rule;
  };

  // Rule 0 -- is there any evidence at all?
  if (input.assessment.quality == EvidenceQuality::Unknown || input.reported == 0u) {
    record(RuleId::EvidencePresence, true, "no evidence has been recorded for this queue");
    conclude(PressureState::Unknown, RuleId::EvidencePresence);
    result.explanation_digest = explanation_digest(result);
    return result;
  }
  record(RuleId::EvidencePresence, false, "evidence present: " + describe(input.assessment));

  // Rule 1 -- do peer sources disagree?
  if (input.peer_conflict || input.assessment.quality == EvidenceQuality::Conflicting) {
    record(RuleId::EvidenceConflict, true,
           "equally authoritative sources disagree beyond the configured tolerance");
    conclude(PressureState::Conflicting, RuleId::EvidenceConflict);
    result.explanation_digest = explanation_digest(result);
    return result;
  }
  record(RuleId::EvidenceConflict, false, "no peer disagreement is recorded");

  // Rule 2 -- is the evidence something this runtime interprets?
  if (input.assessment.quality == EvidenceQuality::Unsupported) {
    record(RuleId::EvidenceUnsupported, true,
           "the reported evidence is not interpreted by this runtime");
    conclude(PressureState::Unknown, RuleId::EvidenceUnsupported);
    result.explanation_digest = explanation_digest(result);
    return result;
  }
  record(RuleId::EvidenceUnsupported, false, "evidence is supported");

  // Rule 3 -- is the evidence fresh enough to speak for the present?
  if (input.assessment.freshness != Freshness::Fresh) {
    record(RuleId::EvidenceFreshness, true,
           std::string("freshness is ") + std::string(to_string(input.assessment.freshness)) +
               " with age " + unsigned_text(static_cast<std::uint64_t>(
                                    input.assessment.age_ns < 0 ? 0 : input.assessment.age_ns)) +
               "ns; only fresh evidence defines the current state");
    conclude(PressureState::Stale, RuleId::EvidenceFreshness);
    result.explanation_digest = explanation_digest(result);
    return result;
  }
  record(RuleId::EvidenceFreshness, false,
         std::string("evidence is fresh with age ") +
             unsigned_text(static_cast<std::uint64_t>(input.assessment.age_ns < 0
                                                          ? 0
                                                          : input.assessment.age_ns)) +
             "ns");

  // Rule 4 -- were the fields the policy requires actually reported?
  //
  // Two masks are evaluated: every field in required_fields must be present,
  // and at least one field from required_any_fields must be present. The second
  // mask is what lets a policy accept either unit of the same measurement
  // without treating a missing alternative as a missing measurement.
  const FieldMask missing = policy.required_fields & ~input.reported;
  if (missing != 0u) {
    record(RuleId::RequiredFields, true,
           "required fields were not reported: " + field_list(missing));
    conclude(PressureState::Unknown, RuleId::RequiredFields);
    result.explanation_digest = explanation_digest(result);
    return result;
  }
  if (policy.required_any_fields != 0u &&
      (policy.required_any_fields & input.reported) == 0u) {
    record(RuleId::RequiredFields, true,
           "none of the alternative measurement fields were reported: " +
               field_list(policy.required_any_fields));
    conclude(PressureState::Unknown, RuleId::RequiredFields);
    result.explanation_digest = explanation_digest(result);
    return result;
  }
  record(RuleId::RequiredFields, false, "all required fields were reported");

  // Rule 5 -- is there a measurement and a limit to compare it against?
  result.basis = resolve_pressure_basis(policy, input);
  if (result.basis.kind == PressureBasisKind::None) {
    record(RuleId::PressureBasis, true, "no usable pressure basis: " + result.basis.label);
    conclude(PressureState::Unknown, RuleId::PressureBasis);
    result.explanation_digest = explanation_digest(result);
    return result;
  }
  record(RuleId::PressureBasis, false, "pressure basis is " + result.basis.label,
         result.basis.value.value_or(0), result.basis.limit.value_or(0));

  // Rule 6 -- did traffic get discarded inside the evaluation window?
  if (input.drops.known && input.drops.delta >= policy.absolute.dropping_at_drops) {
    record(RuleId::DroppingEvidence, true,
           "drop counter advanced inside the evaluation window", input.drops.delta,
           policy.absolute.dropping_at_drops);
    conclude(PressureState::Dropping, RuleId::DroppingEvidence);
    result.explanation_digest = explanation_digest(result);
    return result;
  }
  if (!input.drops.known) {
    record(RuleId::DroppingEvidence, false,
           "drop delta is unknown for this window; no drop claim is made");
  } else {
    record(RuleId::DroppingEvidence, false, "no drop evidence inside the evaluation window",
           input.drops.delta, policy.absolute.dropping_at_drops);
  }

  // Rule 7 -- was backpressure asserted inside the evaluation window?
  const bool pause_by_frames = input.pause_frames.known &&
                               input.pause_frames.delta >= policy.absolute.paused_at_pause_frames;
  const bool pause_by_time = input.pause_duration.known &&
                             input.pause_duration.delta >= policy.absolute.paused_at_pause_nanos;
  if (pause_by_frames || pause_by_time) {
    const std::uint64_t operand = pause_by_frames ? input.pause_frames.delta
                                                  : input.pause_duration.delta;
    const std::uint64_t threshold =
        pause_by_frames ? policy.absolute.paused_at_pause_frames
                        : policy.absolute.paused_at_pause_nanos;
    record(RuleId::PauseEvidence, true, "pause evidence was observed inside the window", operand,
           threshold);
    conclude(PressureState::Paused, RuleId::PauseEvidence);
    result.explanation_digest = explanation_digest(result);
    return result;
  }
  record(RuleId::PauseEvidence, false, "no pause evidence inside the evaluation window");

  // Thresholds for the level rules.
  std::optional<std::uint64_t> saturated_threshold;
  std::optional<std::uint64_t> pressured_threshold;
  std::optional<std::uint64_t> elevated_threshold;
  if (result.basis.kind == PressureBasisKind::Absolute) {
    saturated_threshold = policy.absolute.saturated_at;
    pressured_threshold = policy.absolute.pressured_at;
    elevated_threshold = policy.absolute.elevated_at;
  } else {
    const std::uint64_t limit = result.basis.limit.value_or(0);
    const auto saturated = permille_of(limit, policy.relative.saturated_permille);
    const auto pressured = permille_of(limit, policy.relative.pressured_permille);
    const auto elevated = permille_of(limit, policy.relative.elevated_permille);
    if (saturated.has_value()) {
      saturated_threshold = saturated.value();
    }
    if (pressured.has_value()) {
      pressured_threshold = pressured.value();
    }
    if (elevated.has_value()) {
      elevated_threshold = elevated.value();
    }
  }

  const std::uint64_t measurement = result.basis.value.value_or(0);
  const bool marks_indicate_pressure =
      input.marks.known && input.marks.delta >= policy.absolute.marks_indicate_pressure_at;

  // Rule 8 -- is the queue at its limit?
  if (saturated_threshold.has_value() && measurement >= *saturated_threshold) {
    record(RuleId::SaturationLevel, true, "measurement is at or above the saturation threshold",
           measurement, *saturated_threshold);
    conclude(PressureState::Saturated, RuleId::SaturationLevel);
    result.explanation_digest = explanation_digest(result);
    return result;
  }
  record(RuleId::SaturationLevel, false, "measurement is below the saturation threshold",
         measurement, saturated_threshold.value_or(0));

  // Rule 9 -- is the queue under pressure?
  if (pressured_threshold.has_value() && measurement >= *pressured_threshold) {
    record(RuleId::PressureLevel, true, "measurement is at or above the pressure threshold",
           measurement, *pressured_threshold);
    conclude(PressureState::Pressured, RuleId::PressureLevel);
    result.explanation_digest = explanation_digest(result);
    return result;
  }
  if (marks_indicate_pressure) {
    record(RuleId::PressureLevel, true,
           "congestion marks were observed inside the window even though occupancy is below the "
           "pressure threshold",
           input.marks.delta, policy.absolute.marks_indicate_pressure_at);
    conclude(PressureState::Pressured, RuleId::PressureLevel);
    result.explanation_digest = explanation_digest(result);
    return result;
  }
  record(RuleId::PressureLevel, false, "measurement is below the pressure threshold", measurement,
         pressured_threshold.value_or(0));

  // Rule 10 -- is the queue elevated?
  if (elevated_threshold.has_value() && measurement >= *elevated_threshold) {
    record(RuleId::ElevationLevel, true, "measurement is at or above the elevated threshold",
           measurement, *elevated_threshold);
    conclude(PressureState::Elevated, RuleId::ElevationLevel);
    result.explanation_digest = explanation_digest(result);
    return result;
  }
  record(RuleId::ElevationLevel, false, "measurement is below the elevated threshold", measurement,
         elevated_threshold.value_or(0));

  // Rule 11 -- is the queue idle?
  if (measurement == 0u && input.enqueues.known && input.enqueues.delta == 0u) {
    record(RuleId::IdleEvidence, true, "occupancy is zero and no traffic was enqueued", 0, 0);
    conclude(PressureState::Idle, RuleId::IdleEvidence);
    result.explanation_digest = explanation_digest(result);
    return result;
  }
  if (measurement == 0u && !input.enqueues.known) {
    record(RuleId::IdleEvidence, false,
           "occupancy is zero but the enqueue delta is unknown, so idleness is not claimed");
  } else if (measurement != 0u) {
    record(RuleId::IdleEvidence, false, "occupancy is non-zero", measurement, 0);
  } else {
    record(RuleId::IdleEvidence, false, "traffic was enqueued inside the window",
           input.enqueues.delta, 0);
  }

  // Rule 12 -- nothing above matched.
  record(RuleId::DefaultNormal, true,
         "no rule above matched; the queue is operating without observed pressure");
  conclude(PressureState::Normal, RuleId::DefaultNormal);
  result.explanation_digest = explanation_digest(result);
  return result;
}

ClassificationInput make_classification_input(const QueueSample& sample) {
  ClassificationInput input;
  input.queue = sample.queue;
  input.reported = sample.reported;
  input.occupancy_cells = sample.value(SampleField::OccupancyCells);
  input.occupancy_bytes = sample.value(SampleField::OccupancyBytes);
  input.queue_depth_packets = sample.value(SampleField::QueueDepthPackets);
  input.dynamic_threshold_cells = sample.value(SampleField::DynamicThresholdCells);
  input.static_threshold_cells = sample.value(SampleField::StaticThresholdCells);
  input.max_depth_packets = sample.value(SampleField::MaxDepthPackets);
  return input;
}

}  // namespace qobs
