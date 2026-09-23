#include "support/TestHarness.hpp"

#include <string>

#include "qobs/core/Cancellation.hpp"
#include "qobs/core/Checked.hpp"
#include "qobs/core/Crc32c.hpp"
#include "qobs/core/Json.hpp"
#include "qobs/core/LockAudit.hpp"
#include "qobs/core/Text.hpp"
#include "qobs/core/Time.hpp"
#include "qobs/model/Identity.hpp"
#include "qobs/version.hpp"

using namespace qobs;

QOBS_TEST(identity, integral_round_trip) {
  const auto parsed = QueueId::parse("42");
  QOBS_REQUIRE(parsed.has_value());
  QOBS_CHECK_EQ(parsed->value(), 42u);
  QOBS_CHECK_EQ(parsed->to_string(), std::string("42"));
  const auto hex = QueueId::parse("0x2a");
  QOBS_REQUIRE(hex.has_value());
  QOBS_CHECK_EQ(hex->value(), 42u);
}

QOBS_TEST(identity, integral_rejects_overflow_and_junk) {
  QOBS_CHECK(!QueueId::parse("4294967296").has_value());
  QOBS_CHECK(!QueueId::parse("").has_value());
  QOBS_CHECK(!QueueId::parse("-1").has_value());
  QOBS_CHECK(!QueueId::parse("12x").has_value());
  QOBS_CHECK(!TrafficClassId::parse("65536").has_value());
  QOBS_CHECK(TrafficClassId::parse("65535").has_value());
}

QOBS_TEST(identity, name_validation) {
  QOBS_CHECK(!DeviceId::create("").has_value());
  QOBS_CHECK(!DeviceId::create(std::string(129u, 'a')).has_value());
  QOBS_CHECK(!DeviceId::create("bad\nname").has_value());
  QOBS_CHECK(!DeviceId::create("bad|name").has_value());
  QOBS_CHECK(DeviceId::create("leaf-01").has_value());
  // Slashes are ordinary characters in interface names and are allowed; it is
  // only the canonical path separator that a name must not contain.
  QOBS_CHECK(PortId::create("ethernet1/1").has_value());
  QOBS_CHECK(SourceId::create("collector/region-a").has_value());
}

QOBS_TEST(identity, queue_path_round_trip) {
  QueuePath path;
  path.device = DeviceId(std::string("leaf-01"));
  path.port = PortId(std::string("ethernet1/1"));
  path.queue = QueueId::from_raw(7);
  QOBS_CHECK(path.valid());
  QOBS_CHECK_EQ(path.to_string(), std::string("leaf-01|ethernet1/1|7"));
}

QOBS_TEST(identity, queue_path_parse_refuses_ambiguity) {
  const auto path = QueuePath::parse("leaf-01|ethernet1/1|7");
  QOBS_REQUIRE(path.has_value());
  QOBS_CHECK_EQ(path->queue.value(), 7u);
  QOBS_CHECK_EQ(path->port.value(), std::string("ethernet1/1"));
  QOBS_CHECK(!QueuePath::parse("leaf-01|7").has_value());
  QOBS_CHECK(!QueuePath::parse("leaf-01|ethernet1-1|7|8").has_value());
  QOBS_CHECK(!QueuePath::parse("leaf-01|ethernet1-1|notanumber").has_value());
  QOBS_CHECK(!QueuePath::parse("leaf-01/ethernet1-1/7").has_value());
}

QOBS_TEST(checked_math, overflow_is_reported_not_wrapped) {
  QOBS_CHECK(checked::add_u64(1u, 2u).value() == 3u);
  QOBS_CHECK(!checked::add_u64(UINT64_MAX, 1u).has_value());
  QOBS_CHECK(!checked::mul_u64(UINT64_MAX, 2u).has_value());
  QOBS_CHECK(checked::mul_u64(0u, UINT64_MAX).value() == 0u);
  QOBS_CHECK(checked::sub_u64(5u, 5u).value() == 0u);
  QOBS_CHECK(!checked::sub_u64(4u, 5u).has_value());
  QOBS_CHECK(checked::div_ceil_u64(10u, 3u).value() == 4u);
  QOBS_CHECK(!checked::div_ceil_u64(10u, 0u).has_value());
  QOBS_CHECK(!checked::add_i64(INT64_MAX, 1).has_value());
  QOBS_CHECK(!checked::sub_i64(INT64_MIN, 1).has_value());
  QOBS_CHECK(checked::narrow_u<std::uint8_t>(255u).has_value());
  QOBS_CHECK(!checked::narrow_u<std::uint8_t>(256u).has_value());
}

QOBS_TEST(crc32c, known_check_vector) {
  QOBS_CHECK_EQ(crc32c(std::string_view("123456789")), 0xE3069283u);
  QOBS_CHECK_EQ(crc32c(std::string_view("")), 0x00000000u);
}

QOBS_TEST(crc32c, incremental_matches_oneshot) {
  const std::string text = "queue observatory integrity check";
  Crc32c incremental;
  incremental.update(std::string_view(text.data(), 7u));
  incremental.update(std::string_view(text.data() + 7u, text.size() - 7u));
  QOBS_CHECK_EQ(incremental.value(), crc32c(std::string_view(text)));
  QOBS_CHECK_EQ(incremental.bytes_processed(), static_cast<std::uint64_t>(text.size()));
}

QOBS_TEST(text_util, parsing_is_strict) {
  std::uint64_t value = 0;
  QOBS_CHECK(text::parse_u64("0", value) && value == 0u);
  QOBS_CHECK(!text::parse_u64(" 1", value));
  QOBS_CHECK(!text::parse_u64("1 ", value));
  QOBS_CHECK(!text::parse_u64("+1", value));
  QOBS_CHECK(!text::parse_u64("18446744073709551616", value));
  std::int64_t signed_value = 0;
  QOBS_CHECK(text::parse_i64("-9223372036854775808", signed_value));
  QOBS_CHECK_EQ(signed_value, INT64_MIN);
  QOBS_CHECK(!text::parse_i64("9223372036854775808", signed_value));
}

QOBS_TEST(text_util, escaping_and_sanitising) {
  QOBS_CHECK_EQ(text::escape_json("a\"b\\c"), std::string("a\\\"b\\\\c"));
  QOBS_CHECK_EQ(text::escape_json(std::string("x\ny", 3u)), std::string("x\\ny"));
  QOBS_CHECK_EQ(text::sanitize_for_display(std::string("a\001b", 3u), 16u), std::string("a.b"));
  QOBS_CHECK_EQ(text::sanitize_for_display("abcdef", 3u), std::string("abc..."));
}

QOBS_TEST(json, round_trip_and_rejection) {
  const auto ok = parse_json("{\"a\":1,\"b\":[true,null,\"x\"]}", JsonLimits{});
  QOBS_REQUIRE(ok.has_value());
  QOBS_CHECK(ok->is_object());
  QOBS_CHECK_EQ(ok->size(), 2u);
  const JsonValue* a = ok->find("a");
  QOBS_REQUIRE(a != nullptr);
  QOBS_CHECK(a->is_uint());
  QOBS_CHECK_EQ(a->as_uint(), 1u);

  QOBS_CHECK(!parse_json("{} trailing", JsonLimits{}).has_value());
  QOBS_CHECK(!parse_json("{\"a\":1,\"a\":2}", JsonLimits{}).has_value());
  QOBS_CHECK(!parse_json("{", JsonLimits{}).has_value());
  QOBS_CHECK(!parse_json("{\"a\":01}", JsonLimits{}).has_value());
  QOBS_CHECK(!parse_json("{\"a\":1,}", JsonLimits{}).has_value());
  QOBS_CHECK(!parse_json("\"\\q\"", JsonLimits{}).has_value());
}

QOBS_TEST(json, limits_are_enforced) {
  JsonLimits limits;
  limits.max_depth = 3;
  QOBS_CHECK(!parse_json("[[[[1]]]]", limits).has_value());
  limits = JsonLimits{};
  limits.max_nodes = 4;
  QOBS_CHECK(!parse_json("[1,2,3,4,5]", limits).has_value());
  limits = JsonLimits{};
  limits.max_string_bytes = 3;
  QOBS_CHECK(!parse_json("\"abcd\"", limits).has_value());
  limits = JsonLimits{};
  limits.max_document_bytes = 4;
  QOBS_CHECK(!parse_json("[1,2,3,4,5]", limits).has_value());
}

QOBS_TEST(json, writer_is_deterministic_and_guards_misuse) {
  JsonWriter first;
  QOBS_CHECK_STATUS(first.begin_object());
  QOBS_CHECK_STATUS(first.member_string("z", "1"));
  QOBS_CHECK_STATUS(first.member_u64("a", 2));
  QOBS_CHECK_STATUS(first.key("list"));
  QOBS_CHECK_STATUS(first.begin_array());
  QOBS_CHECK_STATUS(first.value_u64(1));
  QOBS_CHECK_STATUS(first.value_bool(true));
  QOBS_CHECK_STATUS(first.value_null());
  QOBS_CHECK_STATUS(first.end_array());
  QOBS_CHECK_STATUS(first.end_object());
  QOBS_CHECK_EQ(first.buffer(), std::string("{\"z\":\"1\",\"a\":2,\"list\":[1,true,null]}"));

  JsonWriter broken;
  QOBS_CHECK_STATUS(broken.begin_object());
  QOBS_CHECK(!broken.value_u64(1).ok());
  QOBS_CHECK(!broken.end_array().ok());
}

QOBS_TEST(json, fixed_point_is_exact) {
  JsonWriter writer;
  QOBS_CHECK_STATUS(writer.begin_array());
  QOBS_CHECK_STATUS(writer.value_fixed(1u, 3u));
  QOBS_CHECK_STATUS(writer.value_fixed(1000u, 3u));
  QOBS_CHECK_STATUS(writer.value_fixed(7u, 0u));
  QOBS_CHECK_STATUS(writer.end_array());
  QOBS_CHECK_EQ(writer.buffer(), std::string("[0.001,1.000,7]"));
}

QOBS_TEST(time_model, wall_format_round_trip) {
  QOBS_CHECK_EQ(format_wall_utc(WallTime{0}), std::string("1970-01-01T00:00:00.000000000Z"));
  QOBS_CHECK_EQ(format_wall_utc(WallTime{1500000000LL}), std::string("1970-01-01T00:00:01.500000000Z"));
  const auto parsed = parse_wall_utc("2026-02-17T12:34:56.123456789Z");
  QOBS_REQUIRE(parsed.has_value());
  QOBS_CHECK_EQ(format_wall_utc(parsed.value()), std::string("2026-02-17T12:34:56.123456789Z"));
  QOBS_CHECK(!parse_wall_utc("2026-02-17T12:34:56").has_value());
  QOBS_CHECK(!parse_wall_utc("2026-13-17T12:34:56Z").has_value());
  QOBS_CHECK(!parse_wall_utc("2026-02-17T12:34:56.1234567890Z").has_value());
}

QOBS_TEST(time_model, clock_domains_are_never_compared) {
  ObservationTime left;
  left.ns = 100;
  left.domain = ClockDomainId(std::string("a"));
  ObservationTime right;
  right.ns = 200;
  right.domain = ClockDomainId(std::string("b"));
  QOBS_CHECK_EQ(compare_observation_times(left, right).comparability,
                ClockComparability::DifferentDomains);
  ObservationTime unknown;
  unknown.ns = 300;
  QOBS_CHECK_EQ(compare_observation_times(left, unknown).comparability,
                ClockComparability::UnknownDomain);
  right.domain = left.domain;
  const TimeComparison same = compare_observation_times(left, right);
  QOBS_CHECK(same.comparable());
  QOBS_CHECK_EQ(same.ordering, -1);
}

QOBS_TEST(time_model, manual_clock_moves_only_when_told) {
  ManualClock clock(SteadyTime{1000});
  QOBS_CHECK_EQ(clock.now().steady.ns, 1000);
  clock.advance(500);
  QOBS_CHECK_EQ(clock.now().steady.ns, 1500);
  // The manual clock starts its wall clock at the epoch and moves it by the
  // same delta, so a test can reason about both without a real clock.
  QOBS_CHECK_EQ(clock.now().wall.ns, 500);
  clock.set_wall(WallTime{1700000000000000000LL});
  QOBS_CHECK_EQ(clock.now().wall.ns, 1700000000000000000LL);
  QOBS_CHECK_EQ(clock.now().steady.ns, 1500);
  QOBS_CHECK_EQ(format_steady(SteadyTime{1500}), std::string("steady+1500ns"));
  QOBS_CHECK_EQ(parse_steady("steady+1500ns").value().ns, 1500);
  QOBS_CHECK(!parse_steady("1500").has_value());
}

QOBS_TEST(lock_audit, ordered_acquisition_is_clean) {
  LockAudit::reset();
  {
    LockRankGuard outer(kRankSources, "sources");
    LockRankGuard middle(kRankStore, "store");
    LockRankGuard leaf(kRankLeaf, "leaf");
  }
  QOBS_CHECK_EQ(LockAudit::total_violations(), 0u);
  QOBS_CHECK(!LockAudit::holds_any());
}

QOBS_TEST(lock_audit, descending_acquisition_is_detected) {
  LockAudit::reset();
  {
    LockRankGuard store(kRankStore, "store");
    LockRankGuard sources(kRankSources, "sources");
  }
  QOBS_CHECK(LockAudit::descending_acquisitions() >= 1u);
  QOBS_CHECK(LockAudit::total_violations() >= 1u);
  LockAudit::reset();
}

QOBS_TEST(lock_audit, same_rank_nesting_is_detected) {
  LockAudit::reset();
  {
    LockRankGuard first(kRankStore, "store");
    LockRankGuard second(kRankStore, "store");
  }
  QOBS_CHECK(LockAudit::same_rank_acquisitions() >= 1u);
  LockAudit::reset();
}

QOBS_TEST(lock_audit, callback_under_lock_is_detected) {
  LockAudit::reset();
  LockAudit::note_callback("no_lock_held");
  QOBS_CHECK_EQ(LockAudit::callbacks_under_lock(), 0u);
  {
    LockRankGuard store(kRankStore, "store");
    LockAudit::note_callback("under_lock");
  }
  QOBS_CHECK_EQ(LockAudit::callbacks_under_lock(), 1u);
  {
    LockRankGuard leaf(kRankLeaf, "leaf");
    LockAudit::note_callback("leaf_only");
  }
  QOBS_CHECK_EQ(LockAudit::callbacks_under_lock(), 1u);
  QOBS_CHECK(LockAudit::enabled());
  LockAudit::reset();
}

QOBS_TEST(lock_audit, unbalanced_release_is_detected) {
  LockAudit::reset();
  LockAudit::leave(kRankStore);
  QOBS_CHECK(LockAudit::unbalanced_releases() >= 1u);
  LockAudit::reset();
}

QOBS_TEST(cancellation, token_observes_source) {
  StopSource source;
  const StopToken token = source.token();
  QOBS_CHECK(token.can_be_cancelled());
  QOBS_CHECK(!token.stop_requested());
  source.request_stop();
  QOBS_CHECK(token.stop_requested());

  const StopToken detached;
  QOBS_CHECK(!detached.can_be_cancelled());
  QOBS_CHECK(!detached.stop_requested());
}

QOBS_TEST(build, version_is_declared) {
  QOBS_CHECK_EQ(std::string(version_string()), std::string("1.0.0"));
  QOBS_CHECK_EQ(version().major, 1u);
  QOBS_CHECK(!build_description().empty());
  QOBS_CHECK(!build_compiler().empty());
}
