#include "support/TestHarness.hpp"

#include "qobs/policy/Policy.hpp"

using namespace qobs;

QOBS_TEST(policy, default_policy_is_valid) {
  QOBS_CHECK_STATUS(validate_policy(default_pressure_policy()));
  QOBS_CHECK_EQ(default_pressure_policy().version, QOBS_POLICY_VERSION);
  QOBS_CHECK(!default_pressure_policy().name.empty());
}

QOBS_TEST(policy, rejects_incoherent_relative_thresholds) {
  PressurePolicy policy;
  policy.relative.elevated_permille = 900;
  policy.relative.pressured_permille = 800;
  QOBS_CHECK_FAILS(validate_policy(policy));
  policy = PressurePolicy{};
  policy.relative.saturated_permille = 1001;
  QOBS_CHECK_FAILS(validate_policy(policy));
  policy = PressurePolicy{};
  policy.relative.elevated_permille = 0;
  QOBS_CHECK_FAILS(validate_policy(policy));
}

QOBS_TEST(policy, rejects_incoherent_absolute_thresholds) {
  PressurePolicy policy;
  policy.absolute.elevated_at = 100u;
  policy.absolute.pressured_at = 50u;
  QOBS_CHECK_FAILS(validate_policy(policy));
  policy = PressurePolicy{};
  policy.absolute.pressured_at = 100u;
  policy.absolute.saturated_at = 90u;
  QOBS_CHECK_FAILS(validate_policy(policy));
}

QOBS_TEST(policy, rejects_incoherent_windows_and_freshness) {
  PressurePolicy policy;
  policy.evaluation_window_ns = 0;
  QOBS_CHECK_FAILS(validate_policy(policy));
  policy = PressurePolicy{};
  policy.freshness.fresh_within_ns = 10;
  policy.freshness.aging_within_ns = 5;
  QOBS_CHECK_FAILS(validate_policy(policy));
  policy = PressurePolicy{};
  policy.microburst.bucket_ns = policy.microburst.window_ns + 1;
  QOBS_CHECK_FAILS(validate_policy(policy));
  policy = PressurePolicy{};
  policy.microburst.min_covered_buckets = 100000u;
  QOBS_CHECK_FAILS(validate_policy(policy));
}

QOBS_TEST(policy, rejects_incoherent_identity_and_conflict_settings) {
  PressurePolicy policy;
  policy.version = 0;
  QOBS_CHECK_FAILS(validate_policy(policy));
  policy = PressurePolicy{};
  policy.name.clear();
  QOBS_CHECK_FAILS(validate_policy(policy));
  policy = PressurePolicy{};
  policy.required_fields = 0;
  policy.required_any_fields = 0;
  QOBS_CHECK_FAILS(validate_policy(policy));
  policy = PressurePolicy{};
  policy.conflict.relative_permille = 1001;
  QOBS_CHECK_FAILS(validate_policy(policy));
  policy = PressurePolicy{};
  policy.contention.count_pressured = false;
  policy.contention.count_saturated = false;
  policy.contention.count_dropping = false;
  policy.contention.count_paused = false;
  QOBS_CHECK_FAILS(validate_policy(policy));
}

QOBS_TEST(policy, permille_rounds_up_and_is_exact) {
  QOBS_CHECK_EQ(permille_of(1000u, 500u).value(), 500u);
  QOBS_CHECK_EQ(permille_of(999u, 500u).value(), 500u);   // 499.5 rounds up
  QOBS_CHECK_EQ(permille_of(1u, 1u).value(), 1u);         // the smallest non-zero share
  QOBS_CHECK_EQ(permille_of(0u, 950u).value(), 0u);
  QOBS_CHECK_EQ(permille_of(1000u, 1000u).value(), 1000u);
  QOBS_CHECK(!permille_of(1000u, 1001u).has_value());
  QOBS_CHECK(!permille_of(UINT64_MAX, 1000u).has_value());
}

QOBS_TEST(policy, state_parsing_and_severity_ordering) {
  QOBS_CHECK_EQ(parse_pressure_state("dropping").value(), PressureState::Dropping);
  QOBS_CHECK(!parse_pressure_state("healthy").has_value());
  QOBS_CHECK_EQ(std::string(to_string(PressureState::Conflicting)), std::string("conflicting"));
  QOBS_CHECK(pressure_severity_rank(PressureState::Dropping) >
             pressure_severity_rank(PressureState::Saturated));
  QOBS_CHECK(pressure_severity_rank(PressureState::Idle) <
             pressure_severity_rank(PressureState::Normal));
  QOBS_CHECK(pressure_severity_rank(PressureState::Conflicting) >
             pressure_severity_rank(PressureState::Stale));
}
