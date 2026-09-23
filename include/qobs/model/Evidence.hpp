#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "qobs/core/Time.hpp"

namespace qobs {

/// How old a piece of evidence is relative to the runtime clock that received
/// it. Freshness is always computed from receive times, which are monotonic,
/// never from a source-reported timestamp that can jump.
enum class Freshness : std::uint8_t {
  /// No receive time is available, so no age can be computed.
  Unknown = 0,
  Fresh,
  Aging,
  Stale,
  Expired,
};

[[nodiscard]] std::string_view to_string(Freshness value) noexcept;
[[nodiscard]] bool is_fresh(Freshness value) noexcept;

/// Freshness boundaries. The intervals are half-open on the upper end so that a
/// value exactly on a boundary is classified the same way on every platform.
struct FreshnessPolicy {
  Nanos fresh_within_ns{5000000000LL};   // 5 s
  Nanos aging_within_ns{15000000000LL};  // 15 s
  Nanos stale_within_ns{60000000000LL};  // 60 s; beyond this the evidence expires

  friend bool operator==(const FreshnessPolicy&, const FreshnessPolicy&) = default;
};

[[nodiscard]] Freshness assess_freshness(Nanos age_ns, const FreshnessPolicy& policy) noexcept;

/// What kind of evidence a record contains, independent of how old it is.
enum class EvidenceQuality : std::uint8_t {
  Unknown = 0,
  /// Every field the policy requires was present and consistent.
  Complete,
  /// Something required was not reported. Missing is never treated as zero.
  Incomplete,
  /// Equally authoritative sources disagree beyond the configured tolerance.
  Conflicting,
  /// The claim itself is not something this runtime can interpret.
  Unsupported,
};

[[nodiscard]] std::string_view to_string(EvidenceQuality value) noexcept;

/// Where a record came from.
enum class Provenance : std::uint8_t {
  Unknown = 0,
  /// Reported directly by a source and admitted without transformation.
  Observed,
  /// Completed by the class metadata registry; the derived part is marked.
  DerivedFromMetadata,
  /// Produced by correlating several observed records.
  Correlated,
  /// Read back from persistence. Never treated as current on its own.
  RecoveredFromPersistence,
};

[[nodiscard]] std::string_view to_string(Provenance value) noexcept;

/// Declared trust level of a source. Higher authority wins a disagreement, and
/// a source may never lower its own declared authority without being fenced.
enum class SourceAuthority : std::uint8_t {
  Unknown = 0,
  Secondary = 1,
  Primary = 2,
  Authoritative = 3,
};

[[nodiscard]] std::string_view to_string(SourceAuthority value) noexcept;
[[nodiscard]] SourceAuthority parse_source_authority(std::string_view text,
                                                     bool& recognised) noexcept;

/// Orthogonal annotations that qualify an assessment without changing its
/// freshness or quality.
enum class EvidenceFlag : std::uint32_t {
  None = 0,
  OrderAnomaly = 1u << 0u,
  ClockDomainUnknown = 1u << 1u,
  ClockDomainMismatch = 1u << 2u,
  DuplicateSuppressed = 1u << 3u,
  IncarnationChanged = 1u << 4u,
  GenerationChanged = 1u << 5u,
  CoverageInsufficient = 1u << 6u,
  Recovered = 1u << 7u,
  ClassResolvedFromMetadata = 1u << 8u,
  SyntheticTransport = 1u << 9u,
  PartialBatch = 1u << 10u,
  PeerDisagreement = 1u << 11u,
  CounterDiscontinuity = 1u << 12u,
};

using EvidenceFlags = std::uint32_t;

[[nodiscard]] constexpr EvidenceFlags evidence_flag(EvidenceFlag value) noexcept {
  return static_cast<EvidenceFlags>(value);
}
[[nodiscard]] constexpr bool has_flag(EvidenceFlags mask, EvidenceFlag value) noexcept {
  return (mask & evidence_flag(value)) != 0u;
}
[[nodiscard]] constexpr EvidenceFlags with_flag(EvidenceFlags mask, EvidenceFlag value) noexcept {
  return mask | evidence_flag(value);
}
[[nodiscard]] constexpr EvidenceFlags without_flag(EvidenceFlags mask, EvidenceFlag value) noexcept {
  return mask & ~evidence_flag(value);
}

/// Render a flag mask as a deterministic, comma-separated list of names.
[[nodiscard]] std::string describe_flags(EvidenceFlags mask);

/// The complete assessment attached to every observation the runtime reports.
struct EvidenceAssessment {
  Freshness freshness{Freshness::Unknown};
  EvidenceQuality quality{EvidenceQuality::Unknown};
  Provenance provenance{Provenance::Unknown};
  SourceAuthority authority{SourceAuthority::Unknown};
  EvidenceFlags flags{0};
  /// Age measured on the receive clock; negative values are recorded as zero
  /// and flagged as an order anomaly.
  Nanos age_ns{0};
  /// Short, deterministic reason for the assessment.
  std::string reason{};

  friend bool operator==(const EvidenceAssessment&, const EvidenceAssessment&) = default;
};

/// The single rule that decides whether evidence may speak for the present.
///
/// Evidence establishes current pressure only when it is fresh, complete, was
/// not recovered from persistence, and came from a source with at least primary
/// authority. Everything else is still reportable history; it is simply not
/// allowed to define the current state.
[[nodiscard]] bool establishes_current_pressure(const EvidenceAssessment& assessment) noexcept;

/// True when the assessment is explicitly stale, expired, conflicting,
/// incomplete, unsupported or unknown -- the states that must never be reported
/// as a healthy queue.
[[nodiscard]] bool is_degraded(const EvidenceAssessment& assessment) noexcept;

/// Deterministic one-line rendering, e.g.
/// "freshness=fresh quality=complete provenance=observed authority=primary age_ns=12".
[[nodiscard]] std::string describe(const EvidenceAssessment& assessment);

}  // namespace qobs
