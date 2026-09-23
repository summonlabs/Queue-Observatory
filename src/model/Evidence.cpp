#include "qobs/model/Evidence.hpp"

#include <array>

namespace qobs {

std::string_view to_string(Freshness value) noexcept {
  switch (value) {
    case Freshness::Unknown:
      return "unknown";
    case Freshness::Fresh:
      return "fresh";
    case Freshness::Aging:
      return "aging";
    case Freshness::Stale:
      return "stale";
    case Freshness::Expired:
      return "expired";
  }
  return "unknown";
}

bool is_fresh(Freshness value) noexcept { return value == Freshness::Fresh; }

Freshness assess_freshness(Nanos age_ns, const FreshnessPolicy& policy) noexcept {
  if (age_ns < 0) {
    // A negative age means the receive clock moved backwards or a caller
    // supplied a future receive time. Treating it as fresh is conservative in
    // the direction of not hiding evidence, and the caller records an order
    // anomaly flag.
    return Freshness::Fresh;
  }
  if (age_ns < policy.fresh_within_ns) {
    return Freshness::Fresh;
  }
  if (age_ns < policy.aging_within_ns) {
    return Freshness::Aging;
  }
  if (age_ns < policy.stale_within_ns) {
    return Freshness::Stale;
  }
  return Freshness::Expired;
}

std::string_view to_string(EvidenceQuality value) noexcept {
  switch (value) {
    case EvidenceQuality::Unknown:
      return "unknown";
    case EvidenceQuality::Complete:
      return "complete";
    case EvidenceQuality::Incomplete:
      return "incomplete";
    case EvidenceQuality::Conflicting:
      return "conflicting";
    case EvidenceQuality::Unsupported:
      return "unsupported";
  }
  return "unknown";
}

std::string_view to_string(Provenance value) noexcept {
  switch (value) {
    case Provenance::Unknown:
      return "unknown";
    case Provenance::Observed:
      return "observed";
    case Provenance::DerivedFromMetadata:
      return "derived_from_metadata";
    case Provenance::Correlated:
      return "correlated";
    case Provenance::RecoveredFromPersistence:
      return "recovered_from_persistence";
  }
  return "unknown";
}

std::string_view to_string(SourceAuthority value) noexcept {
  switch (value) {
    case SourceAuthority::Unknown:
      return "unknown";
    case SourceAuthority::Secondary:
      return "secondary";
    case SourceAuthority::Primary:
      return "primary";
    case SourceAuthority::Authoritative:
      return "authoritative";
  }
  return "unknown";
}

SourceAuthority parse_source_authority(std::string_view text, bool& recognised) noexcept {
  recognised = true;
  if (text == "unknown") {
    return SourceAuthority::Unknown;
  }
  if (text == "secondary") {
    return SourceAuthority::Secondary;
  }
  if (text == "primary") {
    return SourceAuthority::Primary;
  }
  if (text == "authoritative") {
    return SourceAuthority::Authoritative;
  }
  recognised = false;
  return SourceAuthority::Unknown;
}

std::string describe_flags(EvidenceFlags mask) {
  static constexpr std::array<std::pair<EvidenceFlag, std::string_view>, 13> kNames{{
      {EvidenceFlag::OrderAnomaly, "order_anomaly"},
      {EvidenceFlag::ClockDomainUnknown, "clock_domain_unknown"},
      {EvidenceFlag::ClockDomainMismatch, "clock_domain_mismatch"},
      {EvidenceFlag::DuplicateSuppressed, "duplicate_suppressed"},
      {EvidenceFlag::IncarnationChanged, "incarnation_changed"},
      {EvidenceFlag::GenerationChanged, "generation_changed"},
      {EvidenceFlag::CoverageInsufficient, "coverage_insufficient"},
      {EvidenceFlag::Recovered, "recovered"},
      {EvidenceFlag::ClassResolvedFromMetadata, "class_resolved_from_metadata"},
      {EvidenceFlag::SyntheticTransport, "synthetic_transport"},
      {EvidenceFlag::PartialBatch, "partial_batch"},
      {EvidenceFlag::PeerDisagreement, "peer_disagreement"},
      {EvidenceFlag::CounterDiscontinuity, "counter_discontinuity"},
  }};
  std::string out;
  for (const auto& entry : kNames) {
    if (!has_flag(mask, entry.first)) {
      continue;
    }
    if (!out.empty()) {
      out.push_back(',');
    }
    out.append(entry.second);
  }
  if (out.empty()) {
    out.append("none");
  }
  return out;
}

bool establishes_current_pressure(const EvidenceAssessment& assessment) noexcept {
  if (assessment.freshness != Freshness::Fresh) {
    return false;
  }
  if (assessment.quality != EvidenceQuality::Complete) {
    return false;
  }
  if (assessment.provenance == Provenance::RecoveredFromPersistence) {
    return false;
  }
  if (has_flag(assessment.flags, EvidenceFlag::Recovered)) {
    return false;
  }
  return assessment.authority >= SourceAuthority::Primary;
}

bool is_degraded(const EvidenceAssessment& assessment) noexcept {
  return assessment.freshness != Freshness::Fresh || assessment.quality != EvidenceQuality::Complete;
}

std::string describe(const EvidenceAssessment& assessment) {
  std::string out;
  out.reserve(96u);
  out.append("freshness=");
  out.append(to_string(assessment.freshness));
  out.append(" quality=");
  out.append(to_string(assessment.quality));
  out.append(" provenance=");
  out.append(to_string(assessment.provenance));
  out.append(" authority=");
  out.append(to_string(assessment.authority));
  out.append(" age_ns=");
  out.append(std::to_string(static_cast<long long>(assessment.age_ns)));
  out.append(" flags=");
  out.append(describe_flags(assessment.flags));
  if (!assessment.reason.empty()) {
    out.append(" reason=");
    out.append(assessment.reason);
  }
  return out;
}

}  // namespace qobs
