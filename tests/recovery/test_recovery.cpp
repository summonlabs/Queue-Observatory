#include "support/TestHarness.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "qobs/ingest/Wire.hpp"
#include "qobs/persist/Persistence.hpp"
#include "qobs/runtime/Observatory.hpp"
#include "qobs/runtime/Report.hpp"

using namespace qobs;
using qobs::test::SampleBuilder;

namespace {

std::string document_for(std::uint64_t first_sequence, std::size_t count, std::uint64_t occupancy,
                         const char* incarnation) {
  std::string document;
  for (std::size_t index = 0; index < count; ++index) {
    SampleBuilder builder("leaf-01", "ethernet1/1", 0u, "collector-a", incarnation);
    const Nanos at = 1000 + static_cast<Nanos>(index) * 1000;
    builder.sequence(first_sequence + index);
    builder.received_at(at);
    builder.observed_at(at);
    builder.set(SampleField::OccupancyCells, occupancy);
    builder.set(SampleField::DynamicThresholdCells, 1000u);
    builder.set(SampleField::EnqueuePackets, index + 1u);
    std::string line;
    const Status status = encode_sample(builder.build(), line);
    (void)status;
    document += line;
    document.push_back('\n');
  }
  return document;
}

struct LiveStore {
  std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>(SteadyTime{1000});
  std::unique_ptr<QueueStore> store{};

  LiveStore() {
    HistoryLimits limits;
    limits.max_queues = 16;
    limits.max_events_total = 1024;
    store = std::make_unique<QueueStore>(PressurePolicy{}, limits, QueryLimits{}, clock);
  }
};

}  // namespace

QOBS_TEST(recovery, rebuilt_store_reports_recovered_evidence_as_incomplete_and_stale) {
  LiveStore live;
  IngestReport report;
  QOBS_CHECK_STATUS(ingest_document(*live.store, document_for(1, 4, 800, "boot-1"),
                                    IngestLimits{}, report));
  QOBS_CHECK_EQ(report.admission.accepted, 4u);
  QOBS_CHECK_EQ(live.store->counters().state_transitions, 0u);

  DurableSnapshot snapshot;
  QOBS_CHECK_STATUS(build_snapshot(*live.store, 64u, 256u, snapshot));
  QOBS_CHECK_EQ(snapshot.queues.size(), 1u);
  QOBS_CHECK_EQ(QOBS_FRONT(snapshot.queues).samples.size(), 4u);

  std::vector<std::byte> bytes;
  QOBS_CHECK_STATUS(encode_snapshot(snapshot, PersistenceLimits{}, bytes));
  DurableSnapshot decoded;
  DecodeReport decode_report;
  QOBS_CHECK_STATUS(decode_snapshot(bytes, PersistenceLimits{}, decoded, decode_report));
  QOBS_CHECK(decode_report.clean());

  LiveStore rebuilt;
  RecoverySummary summary;
  QOBS_CHECK_STATUS(apply_recovered_snapshot(*rebuilt.store, decoded, decode_report, summary));
  QOBS_CHECK(summary.recovered);
  QOBS_CHECK_EQ(summary.queues, 1u);
  QOBS_CHECK_EQ(summary.samples, 4u);
  QOBS_CHECK(summary.evidence_marked_stale);

  InspectQuery inspect_query;
  InspectResult inspected;
  QOBS_CHECK_STATUS(rebuilt.store->inspect(inspect_query, inspected));
  QOBS_REQUIRE(inspected.rows.size() == 1u);
  QOBS_CHECK_EQ(QOBS_FRONT(inspected.rows).state, PressureState::Stale);
  QOBS_CHECK_EQ(QOBS_FRONT(inspected.rows).assessment.quality, EvidenceQuality::Incomplete);
  QOBS_CHECK_EQ(QOBS_FRONT(inspected.rows).assessment.provenance,
                Provenance::RecoveredFromPersistence);
  QOBS_CHECK_EQ(QOBS_FRONT(inspected.rows).history_records, 4u);

  ExplainQuery explain_query;
  explain_query.queue = QOBS_FRONT(inspected.rows).queue;
  ExplainResult explained;
  QOBS_CHECK_STATUS(rebuilt.store->explain(explain_query, explained));
  QOBS_CHECK(explained.found);
  QOBS_CHECK_EQ(explained.classification.deciding_rule, RuleId::EvidenceFreshness);
  QOBS_CHECK(explained.explanation.find("stale") != std::string::npos);
}

QOBS_TEST(recovery, recovered_events_are_replayed_into_the_bounded_log) {
  LiveStore live;
  IngestReport report;
  QOBS_CHECK_STATUS(ingest_document(*live.store, document_for(1, 3, 900, "boot-1"),
                                    IngestLimits{}, report));
  DurableSnapshot snapshot;
  QOBS_CHECK_STATUS(build_snapshot(*live.store, 64u, 256u, snapshot));
  QOBS_CHECK(!snapshot.events.empty());

  LiveStore rebuilt;
  RecoverySummary summary;
  DecodeReport decode_report;
  decode_report.magic_valid = true;
  decode_report.header_crc_valid = true;
  decode_report.payload_crc_valid = true;
  decode_report.version_supported = true;
  QOBS_CHECK_STATUS(apply_recovered_snapshot(*rebuilt.store, snapshot, decode_report, summary));
  QOBS_CHECK(summary.events > 0u);
  EventQuery query;
  EventResult events;
  QOBS_CHECK_STATUS(rebuilt.store->events(query, events));
  QOBS_CHECK(events.total_matched > 0u);
}

QOBS_TEST(recovery, a_truncated_segment_still_yields_its_readable_prefix) {
  LiveStore live;
  IngestReport report;
  QOBS_CHECK_STATUS(ingest_document(*live.store, document_for(1, 6, 500, "boot-1"),
                                    IngestLimits{}, report));
  DurableSnapshot snapshot;
  QOBS_CHECK_STATUS(build_snapshot(*live.store, 64u, 256u, snapshot));
  std::vector<std::byte> bytes;
  QOBS_CHECK_STATUS(encode_snapshot(snapshot, PersistenceLimits{}, bytes));

  // Keep the header, the runtime identifier and the session record, then stop
  // in the middle of the next record.
  const std::size_t keep = kSegmentHeaderSize + snapshot.runtime_id.size() + 100u;
  std::vector<std::byte> truncated(bytes.begin(),
                                   bytes.begin() + static_cast<std::ptrdiff_t>(keep));
  DurableSnapshot decoded;
  DecodeReport decode_report;
  QOBS_CHECK_STATUS(decode_snapshot(truncated, PersistenceLimits{}, decoded, decode_report));
  QOBS_CHECK(!decode_report.clean());
  QOBS_CHECK(decode_report.truncated);
  QOBS_CHECK(decode_report.records_parsed >= 1u);
  QOBS_CHECK(!decode_report.diagnostic.empty());
}

QOBS_TEST(recovery, restart_cycle_preserves_history_without_promoting_it) {
  const std::string directory = qobs::test::make_temp_directory("recovery-cycle");

  for (int cycle = 0; cycle < 2; ++cycle) {
    ObservatoryConfig config;
    config.runtime.worker_threads = 0;
    config.persistence_directory = std::filesystem::path(directory);
    config.recover_on_start = cycle > 0;
    auto clock = std::make_shared<ManualClock>(SteadyTime{1000 + cycle * 1000});
    auto created = Observatory::create(config, clock);
    QOBS_REQUIRE(created.has_value());
    std::unique_ptr<Observatory> runtime = std::move(created).value();
    QOBS_CHECK_STATUS(runtime->start());

    IngestReport report;
    QOBS_CHECK_STATUS(runtime->ingest_document(
        document_for(1, 3, 400, cycle == 0 ? "boot-1" : "boot-2"), report));
    QOBS_CHECK_EQ(report.admission.accepted, 3u);

    PressureQuery query;
    PressureResult pressure;
    QOBS_CHECK_STATUS(runtime->pressure(query, pressure));
    QOBS_REQUIRE(pressure.rows.size() == 1u);
    if (cycle == 0) {
      QOBS_CHECK_EQ(QOBS_FRONT(pressure.rows).assessment.freshness, Freshness::Fresh);
    } else {
      // The recovered evidence is present, and the fresh sample that follows it
      // is what decides the state.
      QOBS_CHECK_EQ(QOBS_FRONT(pressure.rows).assessment.freshness, Freshness::Fresh);
      QOBS_CHECK_EQ(QOBS_FRONT(pressure.rows).assessment.provenance, Provenance::Observed);
    }

    PersistenceReport persisted;
    QOBS_CHECK_STATUS(runtime->persist_now(persisted));
    QOBS_CHECK_EQ(persisted.segments_written, 1u);
    QOBS_CHECK_STATUS(runtime->stop());
  }
}

QOBS_TEST(recovery, recovery_reports_a_damaged_store_and_still_starts) {
  const std::string directory = qobs::test::make_temp_directory("recovery-damaged");

  {
    ObservatoryConfig config;
    config.runtime.worker_threads = 0;
    config.persistence_directory = std::filesystem::path(directory);
    config.recover_on_start = false;
    auto clock = std::make_shared<ManualClock>(SteadyTime{1000});
    auto created = Observatory::create(config, clock);
    QOBS_REQUIRE(created.has_value());
    std::unique_ptr<Observatory> runtime = std::move(created).value();
    QOBS_CHECK_STATUS(runtime->start());
    IngestReport report;
    QOBS_CHECK_STATUS(runtime->ingest_document(document_for(1, 3, 400, "boot-1"), report));
    PersistenceReport persisted;
    QOBS_CHECK_STATUS(runtime->persist_now(persisted));
    QOBS_CHECK_STATUS(runtime->stop());
  }

  // Corrupt every byte of the payload while leaving the header intact, which is
  // the shape of a torn write.
  {
    std::filesystem::path segment;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
      std::uint64_t sequence = 0;
      if (parse_segment_file_name(entry.path().filename().string(), sequence)) {
        segment = entry.path();
      }
    }
    QOBS_REQUIRE(!segment.empty());
    std::string bytes;
    {
      std::ifstream input(segment, std::ios::binary);
      bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    QOBS_REQUIRE(bytes.size() > kSegmentHeaderSize + 8u);
    for (std::size_t index = kSegmentHeaderSize + 4u; index < bytes.size(); ++index) {
      bytes[index] = static_cast<char>(bytes[index] ^ 0x7F);
    }
    std::ofstream output(segment, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }

  {
    ObservatoryConfig config;
    config.runtime.worker_threads = 0;
    config.persistence_directory = std::filesystem::path(directory);
    config.recover_on_start = true;
    auto clock = std::make_shared<ManualClock>(SteadyTime{900000});
    auto created = Observatory::create(config, clock);
    QOBS_REQUIRE(created.has_value());
    std::unique_ptr<Observatory> runtime = std::move(created).value();
    QOBS_CHECK_STATUS(runtime->start());
    const RuntimeStatus status = runtime->status();
    QOBS_CHECK(!status.recovery_diagnostic.empty());
    // Starting must succeed and the runtime must be usable even when the
    // persisted evidence was unusable.
    IngestReport report;
    QOBS_CHECK_STATUS(runtime->ingest_document(document_for(1, 2, 100, "boot-9"), report));
    QOBS_CHECK_EQ(report.admission.accepted, 2u);
    QOBS_CHECK_STATUS(runtime->stop());
  }
}

QOBS_TEST(recovery, persistence_without_a_directory_is_reported_not_ignored) {
  ObservatoryConfig config;
  config.runtime.worker_threads = 0;
  auto created = Observatory::create(config, nullptr);
  QOBS_REQUIRE(created.has_value());
  std::unique_ptr<Observatory> runtime = std::move(created).value();
  QOBS_CHECK_STATUS(runtime->start());
  PersistenceReport report;
  QOBS_CHECK_EQ(runtime->persist_now(report).code(), ErrorCode::NotSupported);
  RecoverySummary summary;
  QOBS_CHECK_EQ(runtime->recover(summary).code(), ErrorCode::NotSupported);
  QOBS_CHECK_STATUS(runtime->stop());
}
