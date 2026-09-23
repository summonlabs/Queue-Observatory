#include "support/TestHarness.hpp"

#include "qobs/model/Evidence.hpp"

using namespace qobs;

QOBS_TEST(evidence, freshness_boundaries_are_half_open) {
  const FreshnessPolicy policy;
  QOBS_CHECK_EQ(assess_freshness(0, policy), Freshness::Fresh);
  QOBS_CHECK_EQ(assess_freshness(policy.fresh_within_ns - 1, policy), Freshness::Fresh);
  QOBS_CHECK_EQ(assess_freshness(policy.fresh_within_ns, policy), Freshness::Aging);
  QOBS_CHECK_EQ(assess_freshness(policy.aging_within_ns - 1, policy), Freshness::Aging);
  QOBS_CHECK_EQ(assess_freshness(policy.aging_within_ns, policy), Freshness::Stale);
  QOBS_CHECK_EQ(assess_freshness(policy.stale_within_ns, policy), Freshness::Expired);
  QOBS_CHECK_EQ(assess_freshness(-5, policy), Freshness::Fresh);
}

QOBS_TEST(evidence, only_fresh_complete_authoritative_evidence_speaks_for_now) {
  EvidenceAssessment assessment;
  assessment.freshness = Freshness::Fresh;
  assessment.quality = EvidenceQuality::Complete;
  assessment.provenance = Provenance::Observed;
  assessment.authority = SourceAuthority::Primary;
  QOBS_CHECK(establishes_current_pressure(assessment));
  QOBS_CHECK(!is_degraded(assessment));

  assessment.freshness = Freshness::Stale;
  QOBS_CHECK(!establishes_current_pressure(assessment));
  QOBS_CHECK(is_degraded(assessment));

  assessment.freshness = Freshness::Fresh;
  assessment.quality = EvidenceQuality::Incomplete;
  QOBS_CHECK(!establishes_current_pressure(assessment));

  assessment.quality = EvidenceQuality::Complete;
  assessment.authority = SourceAuthority::Secondary;
  QOBS_CHECK(!establishes_current_pressure(assessment));

  assessment.authority = SourceAuthority::Authoritative;
  QOBS_CHECK(establishes_current_pressure(assessment));

  assessment.authority = SourceAuthority::Primary;
  assessment.provenance = Provenance::RecoveredFromPersistence;
  QOBS_CHECK(!establishes_current_pressure(assessment));

  assessment.provenance = Provenance::Observed;
  assessment.flags = with_flag(assessment.flags, EvidenceFlag::Recovered);
  QOBS_CHECK(!establishes_current_pressure(assessment));
}

QOBS_TEST(evidence, quality_and_flag_rendering_is_stable) {
  QOBS_CHECK_EQ(std::string(to_string(EvidenceQuality::Conflicting)), std::string("conflicting"));
  QOBS_CHECK_EQ(std::string(to_string(Freshness::Expired)), std::string("expired"));
  QOBS_CHECK_EQ(std::string(to_string(SourceAuthority::Authoritative)),
                std::string("authoritative"));
  EvidenceFlags flags = 0;
  QOBS_CHECK_EQ(describe_flags(flags), std::string("none"));
  flags = with_flag(flags, EvidenceFlag::Recovered);
  flags = with_flag(flags, EvidenceFlag::CoverageInsufficient);
  QOBS_CHECK_EQ(describe_flags(flags), std::string("coverage_insufficient,recovered"));
  flags = without_flag(flags, EvidenceFlag::Recovered);
  QOBS_CHECK_EQ(describe_flags(flags), std::string("coverage_insufficient"));
  QOBS_CHECK(has_flag(flags, EvidenceFlag::CoverageInsufficient));
}

QOBS_TEST(evidence, description_is_deterministic) {
  EvidenceAssessment assessment;
  assessment.freshness = Freshness::Stale;
  assessment.quality = EvidenceQuality::Incomplete;
  assessment.provenance = Provenance::RecoveredFromPersistence;
  assessment.authority = SourceAuthority::Secondary;
  assessment.age_ns = 12345;
  assessment.reason = "because";
  const std::string first = describe(assessment);
  const std::string second = describe(assessment);
  QOBS_CHECK_EQ(first, second);
  QOBS_CHECK(first.find("freshness=stale") != std::string::npos);
  QOBS_CHECK(first.find("provenance=recovered_from_persistence") != std::string::npos);
}

QOBS_TEST(evidence, authority_parsing_reports_unrecognised_values) {
  bool recognised = false;
  QOBS_CHECK_EQ(parse_source_authority("primary", recognised), SourceAuthority::Primary);
  QOBS_CHECK(recognised);
  QOBS_CHECK_EQ(parse_source_authority("authoritative", recognised),
                SourceAuthority::Authoritative);
  QOBS_CHECK(recognised);
  (void)parse_source_authority("root", recognised);
  QOBS_CHECK(!recognised);
}
