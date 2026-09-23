#include "support/TestHarness.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "qobs/ingest/Wire.hpp"
#include "qobs/persist/Persistence.hpp"
#include "qobs/runtime/Observatory.hpp"
#include "qobs/runtime/Report.hpp"

using namespace qobs;
using qobs::test::SampleBuilder;

namespace {

std::string build_document(std::uint64_t first_sequence, std::size_t count,
                           std::uint64_t occupancy, std::uint64_t drop_step) {
  std::string document;
  for (std::size_t index = 0; index < count; ++index) {
    SampleBuilder builder("leaf-01", "ethernet1/1", 0u, "collector-a", "boot-1");
    const Nanos at = 1000 + static_cast<Nanos>(index) * 1000;
    builder.sequence(first_sequence + index);
    builder.received_at(at);
    builder.observed_at(at);
    builder.set(SampleField::OccupancyCells, occupancy);
    builder.set(SampleField::DynamicThresholdCells, 1000u);
    builder.set(SampleField::EnqueuePackets, index + 1u);
    builder.set(SampleField::DropPackets, static_cast<std::uint64_t>(index) * drop_step);
    std::string line;
    const Status status = encode_sample(builder.build(), line);
    (void)status;
    document += line;
    document.push_back('\n');
  }
  return document;
}

struct Run {
  ObservatoryConfig config;
  std::shared_ptr<ManualClock> clock = std::make_shared<ManualClock>(SteadyTime{1000});
  std::unique_ptr<Observatory> runtime{};
};

std::unique_ptr<Observatory> start_runtime(ObservatoryConfig config,
                                           const std::shared_ptr<ManualClock>& clock) {
  auto created = Observatory::create(config, clock);
  if (!created.has_value()) {
    return nullptr;
  }
  std::unique_ptr<Observatory> runtime = std::move(created).value();
  if (!runtime->start().ok()) {
    return nullptr;
  }
  return runtime;
}

}  // namespace

QOBS_TEST(end_to_end, ingest_inspect_pressure_history_export) {
  ObservatoryConfig config;
  config.runtime.worker_threads = 0;
  auto clock = std::make_shared<ManualClock>(SteadyTime{1000});
  std::unique_ptr<Observatory> runtime = start_runtime(config, clock);
  QOBS_REQUIRE(runtime != nullptr);

  IngestReport report;
  QOBS_CHECK_STATUS(runtime->ingest_document(build_document(1, 8, 960, 0), report));
  QOBS_CHECK_EQ(report.admission.accepted, 8u);
  QOBS_CHECK_EQ(report.admission.state_changes, 0u);

  PressureQuery pressure_query;
  PressureResult pressure;
  QOBS_CHECK_STATUS(runtime->pressure(pressure_query, pressure));
  QOBS_REQUIRE(pressure.rows.size() == 1u);
  QOBS_CHECK_EQ(QOBS_FRONT(pressure.rows).state, PressureState::Saturated);
  QOBS_CHECK_EQ(QOBS_FRONT(pressure.rows).deciding_rule, RuleId::SaturationLevel);

  // A drop appears in the evaluation window, which outranks saturation.
  const Nanos later = clock->now().steady.ns + 1000000;
  std::string drop_document;
  {
    SampleBuilder builder("leaf-01", "ethernet1/1", 0u, "collector-a", "boot-1");
    builder.sequence(100);
    builder.received_at(later);
    builder.observed_at(later);
    builder.set(SampleField::OccupancyCells, 960u);
    builder.set(SampleField::DynamicThresholdCells, 1000u);
    builder.set(SampleField::EnqueuePackets, 9u);
    builder.set(SampleField::DropPackets, 5u);
    std::string line;
    QOBS_CHECK_STATUS(encode_sample(builder.build(), line));
    drop_document = line + "\n";
  }
  QOBS_CHECK_STATUS(runtime->ingest_document(drop_document, report));
  QOBS_CHECK_EQ(report.admission.accepted, 1u);
  QOBS_CHECK_EQ(report.admission.state_changes, 1u);
  QOBS_CHECK_STATUS(runtime->pressure(pressure_query, pressure));
  QOBS_REQUIRE(pressure.rows.size() == 1u);
  QOBS_CHECK_EQ(QOBS_FRONT(pressure.rows).state, PressureState::Dropping);

  ExportQuery export_query;
  export_query.include_history = true;
  export_query.window.name = "1s";
  export_query.window.duration_ns = 1000000000LL;
  export_query.window.bucket_ns = 125000000LL;
  export_query.window.min_covered_buckets = 1;
  export_query.window.max_buckets = 8;
  ExportResult exported;
  QOBS_CHECK_STATUS(runtime->export_data(export_query, exported));
  QOBS_CHECK_EQ(exported.rows, 1u);
  QOBS_CHECK(exported.history_records > 0u);
  QOBS_CHECK(exported.document.find("window_bucket") != std::string::npos);

  QOBS_CHECK_STATUS(runtime->stop());
}

QOBS_TEST(end_to_end, wire_ingest_is_stamped_by_the_receiving_runtime_and_is_fresh) {
  ObservatoryConfig config;
  config.runtime.worker_threads = 0;
  auto clock = std::make_shared<ManualClock>(SteadyTime{5000000000LL});
  std::unique_ptr<Observatory> runtime = start_runtime(config, clock);
  QOBS_REQUIRE(runtime != nullptr);

  // The canonical format carries no receive timestamp, so every sample that
  // arrives over it must be stamped by this runtime and become fresh evidence.
  std::string document;
  for (std::uint64_t index = 0; index < 3u; ++index) {
    SampleBuilder builder("leaf-01", "ethernet1/1", 0u, "collector-a", "boot-1");
    builder.sequence(index + 1u);
    builder.received_at(0);
    builder.observed_at(static_cast<Nanos>(1000 + index));
    builder.set(SampleField::OccupancyCells, 100u);
    builder.set(SampleField::DynamicThresholdCells, 1000u);
    builder.set(SampleField::EnqueuePackets, index + 1u);
    std::string line;
    QOBS_CHECK_STATUS(encode_sample(builder.build(), line));
    document += line;
    document.push_back('\n');
  }

  IngestReport report;
  QOBS_CHECK_STATUS(runtime->ingest_document(document, report));
  QOBS_CHECK_EQ(report.admission.accepted, 3u);

  PressureQuery query;
  PressureResult pressure;
  QOBS_CHECK_STATUS(runtime->pressure(query, pressure));
  QOBS_REQUIRE(pressure.rows.size() == 1u);
  QOBS_CHECK_EQ(QOBS_FRONT(pressure.rows).assessment.freshness, Freshness::Fresh);
  QOBS_CHECK(establishes_current_pressure(QOBS_FRONT(pressure.rows).assessment));
  QOBS_CHECK_EQ(QOBS_FRONT(pressure.rows).state, PressureState::Normal);
  QOBS_CHECK_STATUS(runtime->stop());
}

QOBS_TEST(end_to_end, identical_input_produces_identical_output) {
  std::vector<std::string> documents;
  for (int run = 0; run < 3; ++run) {
    ObservatoryConfig config;
    config.runtime.worker_threads = 0;
    auto clock = std::make_shared<ManualClock>(SteadyTime{5000});
    std::unique_ptr<Observatory> runtime = start_runtime(config, clock);
    QOBS_REQUIRE(runtime != nullptr);
    IngestReport report;
    QOBS_CHECK_STATUS(runtime->ingest_document(build_document(1, 6, 700, 3), report));
    QOBS_CHECK_EQ(report.admission.accepted, 6u);
    ExportQuery query;
    query.include_history = true;
    query.window.name = "1s";
    query.window.duration_ns = 1000000000LL;
    query.window.bucket_ns = 125000000LL;
    query.window.min_covered_buckets = 1;
    query.window.max_buckets = 8;
    ExportResult exported;
    QOBS_CHECK_STATUS(runtime->export_data(query, exported));
    QOBS_CHECK(!exported.document.empty());
    documents.push_back(exported.document);
    QOBS_CHECK_STATUS(runtime->stop());
  }
  QOBS_REQUIRE(documents.size() == 3u);
  QOBS_CHECK_EQ(documents[0], documents[1]);
  QOBS_CHECK_EQ(documents[1], documents[2]);
}

QOBS_TEST(end_to_end, restarting_marks_persisted_evidence_stale_not_fresh) {
  const std::string directory = qobs::test::make_temp_directory("e2e-restart");

  {
    ObservatoryConfig config;
    config.runtime.worker_threads = 0;
    config.persistence_directory = std::filesystem::path(directory);
    config.recover_on_start = false;
    auto clock = std::make_shared<ManualClock>(SteadyTime{1000});
    std::unique_ptr<Observatory> runtime = start_runtime(config, clock);
    QOBS_REQUIRE(runtime != nullptr);
    IngestReport report;
    QOBS_CHECK_STATUS(runtime->ingest_document(build_document(1, 5, 900, 0), report));
    QOBS_CHECK_EQ(report.admission.accepted, 5u);

    PersistenceReport persisted;
    QOBS_CHECK_STATUS(runtime->persist_now(persisted));
    QOBS_CHECK_EQ(persisted.segments_written, 1u);
    QOBS_CHECK_STATUS(runtime->stop());
  }

  {
    ObservatoryConfig config;
    config.runtime.worker_threads = 0;
    config.persistence_directory = std::filesystem::path(directory);
    config.recover_on_start = true;
    auto clock = std::make_shared<ManualClock>(SteadyTime{900000000});
    std::unique_ptr<Observatory> runtime = start_runtime(config, clock);
    QOBS_REQUIRE(runtime != nullptr);
    const RuntimeStatus status = runtime->status();
    QOBS_CHECK(status.recovered_on_start);
    QOBS_CHECK_EQ(status.queues, 1u);

    PressureQuery query;
    PressureResult pressure;
    QOBS_CHECK_STATUS(runtime->pressure(query, pressure));
    QOBS_REQUIRE(pressure.rows.size() == 1u);
    // The decisive property: recovered evidence can never present itself as the
    // current state of the queue.
    QOBS_CHECK_EQ(QOBS_FRONT(pressure.rows).state, PressureState::Stale);
    QOBS_CHECK_NE(QOBS_FRONT(pressure.rows).assessment.freshness, Freshness::Fresh);
    QOBS_CHECK_EQ(QOBS_FRONT(pressure.rows).assessment.provenance,
                  Provenance::RecoveredFromPersistence);
    QOBS_CHECK(has_flag(QOBS_FRONT(pressure.rows).assessment.flags, EvidenceFlag::Recovered));

    InspectQuery inspect_query;
    InspectResult inspected;
    QOBS_CHECK_STATUS(runtime->inspect(inspect_query, inspected));
    QOBS_REQUIRE(inspected.rows.size() == 1u);
    QOBS_CHECK_EQ(QOBS_FRONT(inspected.rows).state, PressureState::Stale);
    QOBS_CHECK_EQ(QOBS_FRONT(inspected.rows).history_records, 5u);

    QOBS_CHECK_STATUS(runtime->stop());
  }
}

QOBS_TEST(end_to_end, fresh_evidence_after_recovery_restores_a_live_state) {
  const std::string directory = qobs::test::make_temp_directory("e2e-refresh");
  ObservatoryConfig config;
  config.runtime.worker_threads = 0;
  config.persistence_directory = std::filesystem::path(directory);
  config.recover_on_start = false;
  auto clock = std::make_shared<ManualClock>(SteadyTime{1000});
  std::unique_ptr<Observatory> runtime = start_runtime(config, clock);
  QOBS_REQUIRE(runtime != nullptr);
  IngestReport report;
  QOBS_CHECK_STATUS(runtime->ingest_document(build_document(1, 3, 100, 0), report));
  PersistenceReport persisted;
  QOBS_CHECK_STATUS(runtime->persist_now(persisted));
  QOBS_CHECK_STATUS(runtime->stop());

  ObservatoryConfig second = config;
  second.recover_on_start = true;
  auto second_clock = std::make_shared<ManualClock>(SteadyTime{100000000});
  std::unique_ptr<Observatory> recovered = start_runtime(second, second_clock);
  QOBS_REQUIRE(recovered != nullptr);

  PressureQuery query;
  PressureResult pressure;
  QOBS_CHECK_STATUS(recovered->pressure(query, pressure));
  QOBS_REQUIRE(pressure.rows.size() == 1u);
  QOBS_CHECK_EQ(QOBS_FRONT(pressure.rows).state, PressureState::Stale);

  // A new, genuinely fresh observation is the only thing that can restore a
  // live state, and it does so through the ordinary ingest path.
  const Nanos now = second_clock->now().steady.ns;
  std::string fresh;
  {
    SampleBuilder builder("leaf-01", "ethernet1/1", 0u, "collector-a", "boot-2");
    builder.sequence(1);
    builder.received_at(now);
    builder.observed_at(now);
    builder.set(SampleField::OccupancyCells, 100u);
    builder.set(SampleField::DynamicThresholdCells, 1000u);
    builder.set(SampleField::EnqueuePackets, 1u);
    std::string line;
    QOBS_CHECK_STATUS(encode_sample(builder.build(), line));
    fresh = line + "\n";
  }
  IngestReport fresh_report;
  QOBS_CHECK_STATUS(recovered->ingest_document(fresh, fresh_report));
  QOBS_CHECK_EQ(fresh_report.admission.accepted, 1u);
  QOBS_CHECK_STATUS(recovered->pressure(query, pressure));
  QOBS_REQUIRE(pressure.rows.size() == 1u);
  QOBS_CHECK_NE(QOBS_FRONT(pressure.rows).state, PressureState::Stale);
  QOBS_CHECK_EQ(QOBS_FRONT(pressure.rows).assessment.freshness, Freshness::Fresh);
  QOBS_CHECK_STATUS(recovered->stop());
}

QOBS_TEST(end_to_end, sibling_contention_and_microburst_are_reported_with_coverage) {
  ObservatoryConfig config;
  config.runtime.worker_threads = 0;
  // A narrow contention window keeps the bucket layout of this short test
  // meaningful; the default five second window would place every sample in a
  // single bucket and the runtime would correctly report insufficient coverage.
  config.policy.contention.window_ns = 250000000LL;
  // A manual clock keeps the window arithmetic reproducible: samples are
  // stamped at instants this test chooses, not at whatever the host clock says.
  auto clock = std::make_shared<ManualClock>(SteadyTime{0});
  std::unique_ptr<Observatory> runtime = start_runtime(config, clock);
  QOBS_REQUIRE(runtime != nullptr);

  // One metadata revision binds both queues to the same scheduling class.
  ClassMetadataTable table;
  table.revision = Revision::from_raw(1);
  table.generation = GenerationId::from_raw(1);
  table.source = SourceId(std::string("cmdb"));
  table.authority = SourceAuthority::Authoritative;
  SchedulingClassDescriptor scheduling;
  scheduling.id = SchedulingClassId(std::string("sp0"));
  scheduling.mode = "strict-priority";
  table.scheduling_classes.push_back(scheduling);
  std::string metadata_line;
  QOBS_CHECK_STATUS(encode_metadata(table, metadata_line));

  IngestReport metadata_report;
  QOBS_CHECK_STATUS(runtime->ingest_document(metadata_line + "\n", metadata_report));
  QOBS_CHECK_EQ(metadata_report.decode.metadata_records, 1u);

  // Both queues are observed at the same instants, which is what makes the
  // sibling correlation meaningful: the runtime only ever correlates evidence
  // it actually holds for the same bucket. Sequences are strictly increasing
  // per source across both queues, which is what a real source does.
  std::uint64_t sequence = 0;
  constexpr std::size_t kRounds = 6u;
  clock->advance(1000000LL);
  for (std::size_t index = 0; index < kRounds; ++index) {
    for (std::uint32_t queue_index = 0; queue_index < 2u; ++queue_index) {
      ++sequence;
      SampleBuilder builder("leaf-01", "ethernet1/1", queue_index, "collector-a", "boot-1");
      // The receive time is left unset: the canonical format carries no receive
      // timestamp, so the runtime stamps each record with its own clock, which
      // this test controls.
      builder.sequence(sequence);
      builder.received_at(0);
      builder.observed_at(static_cast<Nanos>(1000 + index * 1000u));
      builder.set(SampleField::OccupancyCells, (index >= 2u && index <= 4u) ? 990u : 100u);
      builder.set(SampleField::DynamicThresholdCells, 1000u);
      builder.set(SampleField::EnqueuePackets, index + 1u);
      builder.classes(static_cast<std::uint16_t>(queue_index), "sp0");
      std::string line;
      QOBS_CHECK_STATUS(encode_sample(builder.build(), line));
      IngestReport report;
      QOBS_CHECK_STATUS(runtime->ingest_document(line + "\n", report));
      QOBS_CHECK_EQ(report.admission.accepted, 1u);
    }
    clock->advance(40000000LL);
  }

  MicroburstQuery burst_query;
  MicroburstResult bursts;
  QOBS_CHECK_STATUS(runtime->microburst(burst_query, bursts));
  QOBS_CHECK_EQ(bursts.queues_examined, 2u);
  QOBS_CHECK(!bursts.records.empty());
  bool saw_burst = false;
  for (const MicroburstRecord& record : bursts.records) {
    QOBS_CHECK(!record.sub_sample_reconstructed);
    QOBS_CHECK_EQ(record.required_buckets, 3u);
    QOBS_CHECK_EQ(record.covered_buckets, 6u);
    QOBS_CHECK_EQ(record.peak, 990u);
    QOBS_CHECK_EQ(record.trough, 100u);
    QOBS_CHECK(record.coverage_sufficient);
    if (record.detected) {
      saw_burst = true;
      QOBS_CHECK(record.coverage_sufficient);
      QOBS_CHECK(record.excursion >= record.required_excursion);
    }
  }
  QOBS_CHECK(saw_burst);

  ContentionQuery contention_query;
  ContentionResult contention;
  QOBS_CHECK_STATUS(runtime->contention(contention_query, contention));
  QOBS_CHECK_EQ(contention.ports_examined, 1u);
  QOBS_CHECK_EQ(contention.pairs_examined, 1u);
  QOBS_CHECK_EQ(contention.pairs_insufficient_coverage, 0u);
  QOBS_REQUIRE(contention.records.size() == 1u);
  QOBS_CHECK_EQ(QOBS_FRONT(contention.records).scheduling_class,
                SchedulingClassId(std::string("sp0")));
  QOBS_CHECK(QOBS_FRONT(contention.records).ratio_denominator >= 1u);
  QOBS_CHECK(QOBS_FRONT(contention.records).simultaneous_buckets >= 1u);
  QOBS_CHECK(!render_contention_report(contention).empty());
  QOBS_CHECK(!render_microburst_report(bursts).empty());

  QOBS_CHECK_STATUS(runtime->stop());
}

QOBS_TEST(end_to_end, counters_that_wrap_are_explained_not_hidden) {
  ObservatoryConfig config;
  config.runtime.worker_threads = 0;
  config.policy.continuity.width = CounterWidth::Bits32;
  auto clock = std::make_shared<ManualClock>(SteadyTime{1000});
  std::unique_ptr<Observatory> runtime = start_runtime(config, clock);
  QOBS_REQUIRE(runtime != nullptr);

  std::string document;
  const std::vector<std::uint64_t> counters{0xFFFFFFF0ull, 0xFFFFFFF5ull, 4ull, 20ull};
  for (std::size_t index = 0; index < counters.size(); ++index) {
    SampleBuilder builder("leaf-01", "ethernet1/1", 0u, "collector-a", "boot-1");
    const Nanos at = 1000 + static_cast<Nanos>(index) * 1000;
    builder.sequence(index + 1u);
    builder.received_at(at);
    builder.observed_at(at);
    builder.set(SampleField::OccupancyCells, 10u);
    builder.set(SampleField::DynamicThresholdCells, 1000u);
    builder.set(SampleField::EnqueuePackets, counters[index]);
    std::string line;
    QOBS_CHECK_STATUS(encode_sample(builder.build(), line));
    document += line;
    document.push_back('\n');
  }
  IngestReport report;
  QOBS_CHECK_STATUS(runtime->ingest_document(document, report));
  QOBS_CHECK_EQ(report.admission.accepted, 4u);
  QOBS_CHECK_EQ(runtime->status().counters.counter_wraps, 1u);

  EventQuery event_query;
  event_query.kind = EventKind::CounterWrap;
  EventResult events;
  QOBS_CHECK_STATUS(runtime->events(event_query, events));
  QOBS_CHECK_EQ(events.total_matched, 1u);
  QOBS_CHECK(QOBS_FRONT(events.events).detail.find("enqueue_packets") != std::string::npos);
  QOBS_CHECK(!render_events_report(events).empty());
  QOBS_CHECK_STATUS(runtime->stop());
}
