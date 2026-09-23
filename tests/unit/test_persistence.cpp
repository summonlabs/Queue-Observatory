#include "support/TestHarness.hpp"

#include <filesystem>
#include <fstream>
#include <memory>

#include "qobs/core/Crc32c.hpp"
#include "qobs/persist/Persistence.hpp"

using namespace qobs;

namespace {

DurableSnapshot sample_snapshot() {
  DurableSnapshot snapshot;
  snapshot.runtime_id = "qobs-00000000000000aa";
  snapshot.created = ReceiveTime{SteadyTime{1000}, WallTime{1700000000000000000LL}};
  snapshot.policy_version = 1;
  snapshot.policy_name = "default";

  DurableQueueState queue;
  queue.queue.device = DeviceId(std::string("leaf-01"));
  queue.queue.port = PortId(std::string("ethernet1/1"));
  queue.queue.queue = QueueId::from_raw(3);
  queue.classes_known = true;
  queue.classes.traffic_class = TrafficClassId::from_raw(3);
  queue.classes.scheduling_class = SchedulingClassId(std::string("sp0"));
  queue.classes.origin = AttributeOrigin::ResolvedFromMetadata;
  queue.classes.metadata_revision = Revision::from_raw(4);
  for (int index = 0; index < 3; ++index) {
    DurableSample sample;
    sample.observed.ns = 1000 + index;
    sample.observed.domain = ClockDomainId(std::string("device-clock"));
    sample.received_wall = WallTime{1700000000000000000LL + index};
    sample.received_steady = SteadyTime{1000 + index};
    sample.source = SourceId(std::string("collector-a"));
    sample.generation = GenerationId::from_raw(1);
    sample.sequence = SourceSequence::from_raw(static_cast<std::uint64_t>(index + 1));
    sample.authority = SourceAuthority::Primary;
    sample.reported = field_bit(SampleField::OccupancyCells) |
                      field_bit(SampleField::DynamicThresholdCells) |
                      field_bit(SampleField::DropPackets);
    sample.values[static_cast<std::size_t>(SampleField::OccupancyCells)] =
        static_cast<std::uint64_t>(100 + index);
    sample.values[static_cast<std::size_t>(SampleField::DynamicThresholdCells)] = 1000;
    sample.values[static_cast<std::size_t>(SampleField::DropPackets)] =
        static_cast<std::uint64_t>(index);
    sample.state = PressureState::Normal;
    sample.freshness = Freshness::Fresh;
    sample.quality = EvidenceQuality::Complete;
    sample.age_ns = 10;
    queue.samples.push_back(sample);
  }
  snapshot.queues.push_back(queue);

  EventRecord event;
  event.kind = EventKind::SampleAccepted;
  event.severity = Severity::Info;
  event.received = ReceiveTime{SteadyTime{1000}, WallTime{1700000000000000000LL}};
  event.observed.ns = 1000;
  event.observed.domain = ClockDomainId(std::string("device-clock"));
  event.queue = queue.queue;
  event.source = SourceId(std::string("collector-a"));
  event.sequence = SourceSequence::from_raw(1);
  event.detail = "accepted";
  snapshot.events.push_back(event);

  SourceRecord source;
  source.id = SourceId(std::string("collector-a"));
  source.authority = SourceAuthority::Primary;
  source.declared_authority = SourceAuthority::Primary;
  source.incarnation = IncarnationId(std::string("boot-1"));
  source.incarnation_ordinal = 1;
  source.last_sequence = SourceSequence::from_raw(3);
  source.generation = GenerationId::from_raw(1);
  source.clock_domain = ClockDomainId(std::string("device-clock"));
  source.recent_incarnations.emplace_back(source.incarnation, 1);
  snapshot.sources.push_back(source);

  snapshot.has_metadata = true;
  snapshot.metadata.revision = Revision::from_raw(4);
  snapshot.metadata.generation = GenerationId::from_raw(1);
  snapshot.metadata.source = SourceId(std::string("cmdb"));
  snapshot.metadata.authority = SourceAuthority::Authoritative;
  SchedulingClassDescriptor scheduling;
  scheduling.id = SchedulingClassId(std::string("sp0"));
  scheduling.mode = "strict-priority";
  scheduling.weight = static_cast<std::uint64_t>(1);
  snapshot.metadata.scheduling_classes.push_back(scheduling);
  TrafficClassDescriptor traffic;
  traffic.id = TrafficClassId::from_raw(3);
  traffic.name = "lossless";
  traffic.dscp = static_cast<std::uint16_t>(24);
  traffic.priority_code_point = static_cast<std::uint16_t>(3);
  traffic.scheduling_class = SchedulingClassId(std::string("sp0"));
  snapshot.metadata.traffic_classes.push_back(traffic);
  QueueClassBinding binding;
  binding.queue = queue.queue;
  binding.traffic_class = TrafficClassId::from_raw(3);
  binding.scheduling_class = SchedulingClassId(std::string("sp0"));
  snapshot.metadata.bindings.push_back(binding);
  return snapshot;
}

/// Encode and fail the calling test when encoding did not succeed. Returns an
/// empty vector in that case so that callers can assert on it.
std::vector<std::byte> encode(const DurableSnapshot& snapshot) {
  std::vector<std::byte> bytes;
  const Status status = encode_snapshot(snapshot, PersistenceLimits{}, bytes);
  if (!status.ok()) {
    QOBS_CHECK(false);
    bytes.clear();
  }
  return bytes;
}

}  // namespace

QOBS_TEST(persistence, round_trip_preserves_every_record) {
  const DurableSnapshot original = sample_snapshot();
  const std::vector<std::byte> bytes = encode(original);
  DurableSnapshot decoded;
  DecodeReport report;
  QOBS_CHECK_STATUS(decode_snapshot(bytes, PersistenceLimits{}, decoded, report));
  QOBS_CHECK(report.clean());
  QOBS_CHECK_EQ(report.records_parsed, report.records_declared);
  QOBS_CHECK_EQ(decoded.queues.size(), 1u);
  QOBS_CHECK_EQ(QOBS_FRONT(decoded.queues).samples.size(), 3u);
  QOBS_CHECK_EQ(QOBS_FRONT(decoded.queues).samples.at(2).values[static_cast<std::size_t>(
                    SampleField::OccupancyCells)],
                102u);
  QOBS_CHECK_EQ(QOBS_FRONT(decoded.queues).classes.origin, AttributeOrigin::ResolvedFromMetadata);
  QOBS_CHECK_EQ(QOBS_FRONT(decoded.queues).classes.metadata_revision.value(), 4u);
  QOBS_CHECK_EQ(decoded.events.size(), 1u);
  QOBS_CHECK_EQ(QOBS_FRONT(decoded.events).kind, EventKind::SampleAccepted);
  QOBS_CHECK_EQ(decoded.sources.size(), 1u);
  QOBS_CHECK(decoded.has_metadata);
  QOBS_CHECK_EQ(decoded.metadata.traffic_classes.size(), 1u);
  QOBS_CHECK_EQ(QOBS_FRONT(decoded.metadata.traffic_classes).dscp.value(), 24u);
  QOBS_CHECK_EQ(decoded.metadata.bindings.size(), 1u);
}

QOBS_TEST(persistence, encoding_is_byte_for_byte_deterministic) {
  const DurableSnapshot original = sample_snapshot();
  const std::vector<std::byte> first = encode(original);
  const std::vector<std::byte> second = encode(original);
  QOBS_CHECK(first == second);
}

QOBS_TEST(persistence, corrupted_magic_is_refused) {
  std::vector<std::byte> bytes = encode(sample_snapshot());
  bytes[1] = std::byte{'X'};
  DurableSnapshot decoded;
  DecodeReport report;
  const Status status = decode_snapshot(bytes, PersistenceLimits{}, decoded, report);
  QOBS_CHECK(!status.ok());
  QOBS_CHECK_EQ(status.code(), ErrorCode::IntegrityFailure);
  QOBS_CHECK(!report.magic_valid);
}

QOBS_TEST(persistence, corrupted_header_is_refused) {
  std::vector<std::byte> bytes = encode(sample_snapshot());
  bytes[40] = static_cast<std::byte>(std::to_integer<std::uint8_t>(bytes[40]) ^ 0xFFu);
  DurableSnapshot decoded;
  DecodeReport report;
  const Status status = decode_snapshot(bytes, PersistenceLimits{}, decoded, report);
  QOBS_CHECK(!status.ok());
  QOBS_CHECK_EQ(status.code(), ErrorCode::IntegrityFailure);
  QOBS_CHECK(report.magic_valid);
  QOBS_CHECK(!report.header_crc_valid);
}

QOBS_TEST(persistence, unsupported_version_is_refused) {
  std::vector<std::byte> bytes = encode(sample_snapshot());
  // The header is protected by its own checksum, so a version test has to
  // present a header that is internally consistent but declares a version this
  // build does not read.
  bytes[8] = std::byte{9};
  {
    const std::span<const std::byte> header(bytes.data(), kSegmentHeaderSize);
    std::vector<std::byte> copy(header.begin(), header.end());
    copy[16] = std::byte{0};
    copy[17] = std::byte{0};
    copy[18] = std::byte{0};
    copy[19] = std::byte{0};
    const std::uint32_t crc = crc32c(copy);
    bytes[16] = static_cast<std::byte>(crc & 0xFFu);
    bytes[17] = static_cast<std::byte>((crc >> 8u) & 0xFFu);
    bytes[18] = static_cast<std::byte>((crc >> 16u) & 0xFFu);
    bytes[19] = static_cast<std::byte>((crc >> 24u) & 0xFFu);
  }
  DurableSnapshot decoded;
  DecodeReport report;
  const Status status = decode_snapshot(bytes, PersistenceLimits{}, decoded, report);
  QOBS_CHECK(!status.ok());
  QOBS_CHECK_EQ(status.code(), ErrorCode::VersionMismatch);
}

QOBS_TEST(persistence, truncated_segment_yields_what_survived) {
  const std::vector<std::byte> full = encode(sample_snapshot());
  std::vector<std::byte> bytes(full.begin(),
                               full.begin() + static_cast<std::ptrdiff_t>(full.size() - 20));
  DurableSnapshot decoded;
  DecodeReport report;
  QOBS_CHECK_STATUS(decode_snapshot(bytes, PersistenceLimits{}, decoded, report));
  QOBS_CHECK(report.truncated);
  QOBS_CHECK(report.integrity_failed);
  QOBS_CHECK(!report.clean());
  QOBS_CHECK(!report.diagnostic.empty());
}

QOBS_TEST(persistence, damaged_payload_stops_at_the_damaged_record) {
  std::vector<std::byte> bytes = encode(sample_snapshot());
  // Flip a byte well inside the first record value.
  bytes[kSegmentHeaderSize + 64u] =
      static_cast<std::byte>(std::to_integer<std::uint8_t>(bytes[kSegmentHeaderSize + 64u]) ^ 0x5Au);
  DurableSnapshot decoded;
  DecodeReport report;
  QOBS_CHECK_STATUS(decode_snapshot(bytes, PersistenceLimits{}, decoded, report));
  QOBS_CHECK(report.integrity_failed);
  QOBS_CHECK(report.records_rejected >= 1u);
  QOBS_CHECK(report.records_parsed < report.records_declared);
  QOBS_CHECK(!report.payload_crc_valid);
}

QOBS_TEST(persistence, record_budget_is_enforced) {
  PersistenceLimits limits;
  limits.max_segment_bytes = 128u;
  std::vector<std::byte> bytes;
  QOBS_CHECK_FAILS(encode_snapshot(sample_snapshot(), limits, bytes));
  limits = PersistenceLimits{};
  limits.max_record_bytes = 32u;
  QOBS_CHECK_FAILS(encode_snapshot(sample_snapshot(), limits, bytes));
}

QOBS_TEST(persistence, store_writes_reads_rotates_and_clears) {
  const std::string directory = qobs::test::make_temp_directory("persist");
  auto clock = std::make_shared<ManualClock>(SteadyTime{1000});
  PersistenceLimits limits;
  limits.max_segments = 2;
  PersistenceStore store(std::filesystem::path(directory), limits, clock);
  QOBS_CHECK_STATUS(store.open());

  PersistenceReport report;
  const DurableSnapshot snapshot = sample_snapshot();
  QOBS_CHECK_STATUS(store.write_snapshot(snapshot, report));
  QOBS_CHECK_EQ(report.segments_written, 1u);
  QOBS_CHECK_STATUS(store.write_snapshot(snapshot, report));
  QOBS_CHECK_STATUS(store.write_snapshot(snapshot, report));

  std::vector<SegmentInfo> segments;
  QOBS_CHECK_STATUS(store.list_segments(segments));
  QOBS_CHECK_EQ(segments.size(), 2u);
  QOBS_CHECK_EQ(QOBS_FRONT(segments).sequence, 2u);
  QOBS_CHECK_EQ(QOBS_BACK(segments).sequence, 3u);

  RecoveryOutcome outcome;
  QOBS_CHECK_STATUS(store.recover(outcome));
  QOBS_CHECK(outcome.recovered);
  QOBS_CHECK(outcome.report.clean());
  QOBS_CHECK_EQ(outcome.snapshot.queues.size(), 1u);

  QOBS_CHECK_STATUS(store.clear());
  QOBS_CHECK_STATUS(store.list_segments(segments));
  QOBS_CHECK_EQ(segments.size(), 0u);
}

QOBS_TEST(persistence, damaged_newest_segment_falls_back_and_reports) {
  const std::string directory = qobs::test::make_temp_directory("persist-damaged");
  auto clock = std::make_shared<ManualClock>(SteadyTime{1000});
  PersistenceLimits limits;
  limits.max_segments = 4;
  PersistenceStore store(std::filesystem::path(directory), limits, clock);
  QOBS_CHECK_STATUS(store.open());

  PersistenceReport report;
  const DurableSnapshot snapshot = sample_snapshot();
  QOBS_CHECK_STATUS(store.write_snapshot(snapshot, report));
  QOBS_CHECK_STATUS(store.write_snapshot(snapshot, report));

  std::vector<SegmentInfo> segments;
  QOBS_CHECK_STATUS(store.list_segments(segments));
  QOBS_REQUIRE(segments.size() == 2u);
  {
    std::ofstream stream(QOBS_BACK(segments).path, std::ios::binary | std::ios::trunc);
    stream << "not a segment at all";
  }

  RecoveryOutcome outcome;
  QOBS_CHECK_STATUS(store.recover(outcome));
  QOBS_CHECK(outcome.recovered);
  QOBS_CHECK_EQ(outcome.source.filename().string(), QOBS_FRONT(segments).path.filename().string());
  QOBS_CHECK_EQ(store.stats().damaged_segments_skipped, 1u);
}

QOBS_TEST(persistence, empty_directory_reports_no_recovery) {
  const std::string directory = qobs::test::make_temp_directory("persist-empty");
  auto clock = std::make_shared<ManualClock>(SteadyTime{1000});
  PersistenceStore store(std::filesystem::path(directory), PersistenceLimits{}, clock);
  QOBS_CHECK_STATUS(store.open());
  RecoveryOutcome outcome;
  QOBS_CHECK_STATUS(store.recover(outcome));
  QOBS_CHECK(!outcome.recovered);
  QOBS_CHECK(!outcome.diagnostic.empty());
}

QOBS_TEST(persistence, segment_file_names_round_trip) {
  QOBS_CHECK_EQ(segment_file_name(42u), std::string("segment-00000042.qobs"));
  std::uint64_t sequence = 0;
  QOBS_CHECK(parse_segment_file_name("segment-00000042.qobs", sequence));
  QOBS_CHECK_EQ(sequence, 42u);
  QOBS_CHECK(!parse_segment_file_name("segment-00000042.qobs.tmp", sequence));
  QOBS_CHECK(!parse_segment_file_name("other.qobs", sequence));
}
