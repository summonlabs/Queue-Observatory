#include "support/TestHarness.hpp"

#include <cstdio>
#include <memory>
#include <string>

#include "qobs/ingest/Ingest.hpp"
#include "qobs/ingest/Wire.hpp"
#include "qobs/store/Store.hpp"

using namespace qobs;
using qobs::test::SampleBuilder;

namespace {

std::string sample_line(std::uint64_t sequence, std::uint64_t occupancy) {
  SampleBuilder builder("leaf-01", "ethernet1/1", 0u, "collector-a", "boot-1");
  builder.sequence(sequence).received_at(1000).observed_at(1000);
  builder.set(SampleField::OccupancyCells, occupancy);
  builder.set(SampleField::DynamicThresholdCells, 1000u);
  builder.set(SampleField::EnqueuePackets, 1u);
  std::string line;
  const Status status = encode_sample(builder.build(), line);
  (void)status;
  return line;
}

std::unique_ptr<QueueStore> make_store(std::shared_ptr<ManualClock> clock) {
  HistoryLimits limits;
  limits.max_queues = 16;
  limits.max_events_total = 512;
  return std::make_unique<QueueStore>(PressurePolicy{}, limits, QueryLimits{}, std::move(clock));
}

}  // namespace

QOBS_TEST(ingest, documents_are_decoded_and_applied) {
  auto clock = std::make_shared<ManualClock>(SteadyTime{1000});
  auto store = make_store(clock);
  std::string document;
  document += "# a comment line\n";
  document += "\n";
  document += sample_line(1, 10);
  document.push_back('\n');
  document += sample_line(2, 20);
  document.push_back('\n');

  IngestReport report;
  QOBS_CHECK_STATUS(ingest_document(*store, document, IngestLimits{}, report));
  QOBS_CHECK_EQ(report.decode.lines, 2u);
  QOBS_CHECK_EQ(report.decode.sample_count, 2u);
  QOBS_CHECK_EQ(report.admission.accepted, 2u);
  QOBS_CHECK(report.accepted);
  QOBS_CHECK_EQ(store->queue_count(), 1u);

  ExplainQuery query;
  query.queue.device = DeviceId(std::string("leaf-01"));
  query.queue.port = PortId(std::string("ethernet1/1"));
  query.queue.queue = QueueId::from_raw(0);
  ExplainResult explained;
  QOBS_CHECK_STATUS(store->explain(query, explained));
  QOBS_CHECK(explained.found);
}

QOBS_TEST(ingest, one_bad_line_never_hides_the_rest) {
  auto clock = std::make_shared<ManualClock>(SteadyTime{1000});
  auto store = make_store(clock);
  std::string document;
  document += sample_line(1, 10);
  document.push_back('\n');
  document += "{ this is not json }\n";
  document += "{\"kind\":\"queue_sample\",\"v\":1}\n";
  document += "{\"kind\":\"something_else\",\"v\":1}\n";
  document += "{\"kind\":\"queue_sample\",\"v\":99}\n";
  document += sample_line(2, 20);
  document.push_back('\n');

  IngestReport report;
  QOBS_CHECK_STATUS(ingest_document(*store, document, IngestLimits{}, report));
  QOBS_CHECK_EQ(report.decode.sample_count, 2u);
  QOBS_CHECK_EQ(report.admission.accepted, 2u);
  QOBS_CHECK_EQ(report.decode.malformed, 2u);
  QOBS_CHECK_EQ(report.decode.unsupported, 1u);
  QOBS_CHECK_EQ(report.decode.version_mismatches, 1u);
  QOBS_CHECK_EQ(report.decode.diagnostics.size(), 4u);
}

QOBS_TEST(ingest, unknown_measurement_keys_are_counted_not_ignored_silently) {
  auto clock = std::make_shared<ManualClock>(SteadyTime{1000});
  auto store = make_store(clock);
  std::string line = sample_line(1, 10);
  line.insert(line.size() - 1u, ",\"vendor_private_counter\":7");
  IngestReport report;
  QOBS_CHECK_STATUS(ingest_document(*store, line, IngestLimits{}, report));
  QOBS_CHECK_EQ(report.decode.sample_count, 1u);
  QOBS_CHECK_EQ(report.decode.unknown_keys, 1u);
}

QOBS_TEST(ingest, metadata_records_are_applied_before_samples) {
  auto clock = std::make_shared<ManualClock>(SteadyTime{1000});
  auto store = make_store(clock);
  ClassMetadataTable table;
  table.revision = Revision::from_raw(1);
  table.generation = GenerationId::from_raw(1);
  table.source = SourceId(std::string("cmdb"));
  table.authority = SourceAuthority::Authoritative;
  SchedulingClassDescriptor scheduling;
  scheduling.id = SchedulingClassId(std::string("sp0"));
  scheduling.mode = "strict-priority";
  table.scheduling_classes.push_back(scheduling);
  TrafficClassDescriptor traffic;
  traffic.id = TrafficClassId::from_raw(3);
  traffic.name = "lossless";
  traffic.scheduling_class = SchedulingClassId(std::string("sp0"));
  table.traffic_classes.push_back(traffic);

  std::string document;
  QOBS_CHECK_STATUS(encode_metadata(table, document));
  document.push_back('\n');
  SampleBuilder builder("leaf-01", "ethernet1/1", 0u, "collector-a", "boot-1");
  builder.sequence(1).received_at(1000).observed_at(1000);
  builder.set(SampleField::OccupancyCells, 10u);
  builder.set(SampleField::DynamicThresholdCells, 1000u);
  builder.classes(static_cast<std::uint16_t>(3u), std::nullopt);
  std::string line;
  QOBS_CHECK_STATUS(encode_sample(builder.build(), line));
  document += line;
  document.push_back('\n');

  IngestReport report;
  QOBS_CHECK_STATUS(ingest_document(*store, document, IngestLimits{}, report));
  QOBS_CHECK_EQ(report.decode.metadata_records, 1u);
  QOBS_CHECK_EQ(report.admission.accepted, 1u);
  QOBS_CHECK_EQ(store->counters().metadata_updates, 1u);

  InspectQuery query;
  InspectResult inspected;
  QOBS_CHECK_STATUS(store->inspect(query, inspected));
  QOBS_REQUIRE(inspected.rows.size() == 1u);
  QOBS_CHECK(QOBS_FRONT(inspected.rows).classes.scheduling_class.has_value());
  QOBS_CHECK_EQ(QOBS_FRONT(inspected.rows).classes.origin, AttributeOrigin::ResolvedFromMetadata);
}

QOBS_TEST(ingest, payload_and_sample_budgets_are_enforced) {
  auto clock = std::make_shared<ManualClock>(SteadyTime{1000});
  auto store = make_store(clock);
  std::string document;
  for (std::uint64_t sequence = 1; sequence <= 5; ++sequence) {
    document += sample_line(sequence, sequence * 10u);
    document.push_back('\n');
  }
  IngestLimits limits;
  limits.max_batch_samples = 3;
  IngestReport report;
  QOBS_CHECK_STATUS(ingest_document(*store, document, limits, report));
  QOBS_CHECK_EQ(report.admission.accepted, 3u);
  QOBS_CHECK_EQ(report.decode.malformed, 2u);

  limits = IngestLimits{};
  limits.max_payload_bytes = 16;
  QOBS_CHECK_FAILS(decode_document(document, limits, report.decode));
}

QOBS_TEST(ingest, file_reader_refuses_oversized_documents) {
  const std::string directory = qobs::test::make_temp_directory("ingest");
  const std::string path = directory + "/document.ndjson";
  {
    std::FILE* file = std::fopen(path.c_str(), "wb");
    QOBS_REQUIRE(file != nullptr);
    const std::string content = sample_line(1, 1) + "\n";
    (void)std::fwrite(content.data(), 1u, content.size(), file);
    std::fclose(file);
  }
  std::string document;
  QOBS_CHECK_STATUS(read_document_file(path, 1u << 20, document));
  QOBS_CHECK(!document.empty());
  QOBS_CHECK_FAILS(read_document_file(path, 8u, document));
  QOBS_CHECK_FAILS(read_document_file(directory + "/missing.ndjson", 1024u, document));
}
