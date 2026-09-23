#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "qobs/model/Counter.hpp"
#include "qobs/model/Evidence.hpp"
#include "qobs/model/Identity.hpp"
#include "qobs/model/Sample.hpp"
#include "qobs/policy/Policy.hpp"

namespace qobs {

/// Stable identifiers for the ordered rules. The numbers are part of the
/// explanation format and must not be reordered without a policy version bump.
enum class RuleId : std::uint32_t {
  EvidencePresence = 0,
  EvidenceConflict = 1,
  EvidenceUnsupported = 2,
  EvidenceFreshness = 3,
  RequiredFields = 4,
  PressureBasis = 5,
  DroppingEvidence = 6,
  PauseEvidence = 7,
  SaturationLevel = 8,
  PressureLevel = 9,
  ElevationLevel = 10,
  IdleEvidence = 11,
  DefaultNormal = 12,
};

inline constexpr std::size_t kRuleCount = 13;

[[nodiscard]] std::string_view rule_name(RuleId id) noexcept;

/// What the classifier decided about the measurement basis.
struct PressureBasis {
  PressureBasisKind kind{PressureBasisKind::None};
  SampleField value_field{SampleField::Count};
  std::optional<std::uint64_t> value{};
  SampleField limit_field{SampleField::Count};
  std::optional<std::uint64_t> limit{};
  /// Text label used in explanations, for example "cells:dyn-threshold".
  std::string label{};

  friend bool operator==(const PressureBasis&, const PressureBasis&) = default;
};

/// One entry of the decision trace. A trace entry is appended only for rules
/// that were actually reached, so the trace is a faithful record of the path
/// taken, not a dump of the whole policy.
struct TraceEntry {
  RuleId rule{RuleId::EvidencePresence};
  bool matched{false};
  std::string detail{};
  std::uint64_t operand{0};
  std::uint64_t threshold{0};
};

/// Everything the classifier needs. Every optional that is empty means "not
/// reported", never "zero".
struct ClassificationInput {
  QueuePath queue{};
  EvidenceAssessment assessment{};
  /// Which sample fields were actually reported. Required-field checks are
  /// evaluated against this mask, so an unreported field can never be mistaken
  /// for a reported zero.
  FieldMask reported{0};
  PressureBasisKind preferred_basis{PressureBasisKind::CellsAgainstThreshold};

  std::optional<std::uint64_t> occupancy_cells{};
  std::optional<std::uint64_t> occupancy_bytes{};
  std::optional<std::uint64_t> queue_depth_packets{};
  std::optional<std::uint64_t> dynamic_threshold_cells{};
  std::optional<std::uint64_t> static_threshold_cells{};
  std::optional<std::uint64_t> max_depth_packets{};

  CounterDelta drops{};
  CounterDelta marks{};
  CounterDelta pause_frames{};
  CounterDelta pause_duration{};
  CounterDelta enqueues{};
  CounterDelta dequeues{};

  /// Set when a peer source disagreed beyond tolerance. Evidence quality is
  /// normally already Conflicting in that case; this flag lets a caller force
  /// the state without rewriting the assessment.
  bool peer_conflict{false};
};

struct ClassificationResult {
  PressureState state{PressureState::Unknown};
  /// Canonical text form of the queue path, carried so that an explanation can
  /// be rendered without the caller re-deriving it.
  std::string queue_text{};
  std::vector<TraceEntry> trace{};
  PressureBasis basis{};
  EvidenceAssessment assessment{};
  std::uint32_t policy_version{0};
  std::string policy_name{};
  /// FNV-1a digest of the rendered explanation. Identical inputs always produce
  /// the same digest, which makes determinism directly testable.
  std::uint64_t explanation_digest{0};
  /// The rule that produced the state.
  RuleId deciding_rule{RuleId::EvidencePresence};

  friend bool operator==(const ClassificationResult&, const ClassificationResult&) = default;
};

/// Resolve the measurement basis according to the policy. Exposed separately so
/// that it can be unit-tested and explained on its own.
[[nodiscard]] PressureBasis resolve_pressure_basis(const PressurePolicy& policy,
                                                   const ClassificationInput& input);

/// Deterministic classification. The same input and policy always produce the
/// same state, the same trace and the same digest.
[[nodiscard]] ClassificationResult classify(const PressurePolicy& policy,
                                            const ClassificationInput& input);

/// Helper used by callers to build a classification input from one sample plus
/// the deltas the store computed for it.
[[nodiscard]] ClassificationInput make_classification_input(const QueueSample& sample);

}  // namespace qobs
