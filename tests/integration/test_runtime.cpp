#include "support/TestHarness.hpp"

#include <memory>
#include <string>
#include <vector>

#include "qobs/core/LockAudit.hpp"
#include "qobs/ingest/Wire.hpp"
#include "qobs/runtime/Observatory.hpp"
#include "qobs/runtime/Report.hpp"

using namespace qobs;
using qobs::test::SampleBuilder;

namespace {

std::string document_for(std::uint64_t first_sequence, std::size_t count,
                         std::uint64_t occupancy) {
  std::string document;
  for (std::size_t index = 0; index < count; ++index) {
    SampleBuilder builder("leaf-01", "ethernet1/1", 0u, "collector-a", "boot-1");
    builder.sequence(first_sequence + index);
    builder.received_at(1000 + static_cast<Nanos>(index) * 100);
    builder.observed_at(1000 + static_cast<Nanos>(index) * 100);
    builder.set(SampleField::OccupancyCells, occupancy);
    builder.set(SampleField::DynamicThresholdCells, 1000u);
    builder.set(SampleField::EnqueuePackets, 1u);
    std::string line;
    const Status status = encode_sample(builder.build(), line);
    (void)status;
    document += line;
    document.push_back('\n');
  }
  return document;
}

std::unique_ptr<Observatory> make_runtime(std::size_t workers,
                                          std::shared_ptr<ManualClock> clock) {
  ObservatoryConfig config;
  config.runtime.worker_threads = workers;
  config.history.max_queues = 32;
  config.history.max_events_total = 4096;
  auto created = Observatory::create(config, std::move(clock));
  if (!created.has_value()) {
    return nullptr;
  }
  std::unique_ptr<Observatory> runtime = std::move(created).value();
  const Status started = runtime->start();
  if (!started.ok()) {
    return nullptr;
  }
  return runtime;
}

}  // namespace

QOBS_TEST(runtime, lifecycle_transitions_are_explicit) {
  auto clock = std::make_shared<ManualClock>(SteadyTime{1000});
  ObservatoryConfig config;
  config.runtime.worker_threads = 1;
  auto created = Observatory::create(config, clock);
  QOBS_REQUIRE(created.has_value());
  std::unique_ptr<Observatory> runtime = std::move(created).value();
  QOBS_CHECK_EQ(runtime->status().state, RuntimeState::Created);
  QOBS_CHECK_STATUS(runtime->start());
  QOBS_CHECK_EQ(runtime->status().state, RuntimeState::Running);
  QOBS_CHECK(runtime->running());
  QOBS_CHECK_FAILS(runtime->start());
  QOBS_CHECK_STATUS(runtime->stop());
  QOBS_CHECK_EQ(runtime->status().state, RuntimeState::Stopped);
  QOBS_CHECK_STATUS(runtime->stop());
}

QOBS_TEST(runtime, stopped_runtime_refuses_new_documents) {
  auto clock = std::make_shared<ManualClock>(SteadyTime{1000});
  std::unique_ptr<Observatory> runtime = make_runtime(1, clock);
  QOBS_REQUIRE(runtime != nullptr);
  QOBS_CHECK_STATUS(runtime->stop());
  std::uint64_t ticket = 0;
  const Status status = runtime->submit_document(document_for(1, 1, 10), ticket);
  QOBS_CHECK(!status.ok());
  QOBS_CHECK_EQ(status.code(), ErrorCode::NotRunning);
}

QOBS_TEST(runtime, asynchronous_application_is_ordered_and_worker_independent) {
  std::vector<std::uint64_t> accepted;
  for (const std::size_t workers : {1u, 2u, 4u}) {
    auto clock = std::make_shared<ManualClock>(SteadyTime{1000});
    std::unique_ptr<Observatory> runtime = make_runtime(workers, clock);
    QOBS_REQUIRE(runtime != nullptr);
    std::uint64_t last_ticket = 0;
    for (std::uint64_t index = 0; index < 16u; ++index) {
      std::uint64_t ticket = 0;
      const Status status =
          runtime->submit_document(document_for(index * 4u + 1u, 4u, 100u + index), ticket);
      QOBS_CHECK_STATUS(status);
      QOBS_CHECK(ticket > last_ticket);
      last_ticket = ticket;
    }
    QOBS_CHECK_STATUS(runtime->drain());
    const RuntimeStatus status = runtime->status();
    QOBS_CHECK_EQ(status.applied_documents, 16u);
    QOBS_CHECK_EQ(status.queued_documents, 0u);
    QOBS_CHECK_EQ(status.apply_order_violations, 0u);
    QOBS_CHECK_EQ(status.counters.samples_accepted, 64u);
    accepted.push_back(status.counters.samples_accepted);
    QOBS_CHECK_STATUS(runtime->stop());
  }
  QOBS_REQUIRE(accepted.size() == 3u);
  QOBS_CHECK_EQ(accepted[0], accepted[1]);
  QOBS_CHECK_EQ(accepted[1], accepted[2]);
}

QOBS_TEST(runtime, bounded_queue_reports_busy_rather_than_growing) {
  auto clock = std::make_shared<ManualClock>(SteadyTime{1000});
  ObservatoryConfig config;
  config.runtime.worker_threads = 1;
  config.runtime.max_pending_batches = 2;
  auto created = Observatory::create(config, clock);
  QOBS_REQUIRE(created.has_value());
  std::unique_ptr<Observatory> runtime = std::move(created).value();
  QOBS_CHECK_STATUS(runtime->start());
  std::size_t busy = 0;
  for (std::uint64_t index = 0; index < 64u; ++index) {
    std::uint64_t ticket = 0;
    const Status status = runtime->submit_document(document_for(index + 1u, 1, 10), ticket);
    if (!status.ok()) {
      QOBS_CHECK_EQ(status.code(), ErrorCode::Busy);
      ++busy;
    }
  }
  QOBS_CHECK(busy > 0u);
  QOBS_CHECK_STATUS(runtime->drain());
  QOBS_CHECK(runtime->status().queued_documents <= 2u);
  QOBS_CHECK_STATUS(runtime->stop());
}

QOBS_TEST(runtime, synchronous_mode_applies_immediately) {
  auto clock = std::make_shared<ManualClock>(SteadyTime{1000});
  std::unique_ptr<Observatory> runtime = make_runtime(0, clock);
  QOBS_REQUIRE(runtime != nullptr);
  std::uint64_t ticket = 0;
  QOBS_CHECK_STATUS(runtime->submit_document(document_for(1, 3, 500), ticket));
  QOBS_CHECK_EQ(runtime->status().applied_documents, 1u);
  QOBS_CHECK_EQ(runtime->status().counters.samples_accepted, 3u);
  QOBS_CHECK_STATUS(runtime->stop());
}

QOBS_TEST(runtime, queries_and_reports_are_available_through_the_facade) {
  auto clock = std::make_shared<ManualClock>(SteadyTime{1000});
  std::unique_ptr<Observatory> runtime = make_runtime(1, clock);
  QOBS_REQUIRE(runtime != nullptr);
  IngestReport report;
  QOBS_CHECK_STATUS(runtime->ingest_document(document_for(1, 4, 800), report));
  QOBS_CHECK_EQ(report.admission.accepted, 4u);

  InspectQuery inspect_query;
  InspectResult inspected;
  QOBS_CHECK_STATUS(runtime->inspect(inspect_query, inspected));
  QOBS_CHECK_EQ(inspected.rows.size(), 1u);
  QOBS_CHECK_EQ(QOBS_FRONT(inspected.rows).state, PressureState::Pressured);
  QOBS_CHECK(!render_inspect_table(inspected, true).empty());

  HistoryQuery history_query;
  history_query.queue = QOBS_FRONT(inspected.rows).queue;
  history_query.window.name = "1s";
  history_query.window.duration_ns = 1000000000LL;
  history_query.window.bucket_ns = 125000000LL;
  history_query.window.min_covered_buckets = 1;
  history_query.window.max_buckets = 8;
  HistoryResult history;
  QOBS_CHECK_STATUS(runtime->history(history_query, history));
  QOBS_CHECK(history.found);
  QOBS_CHECK_EQ(history.records_retained, 4u);
  QOBS_CHECK(!render_history_report(history).empty());

  ExplainQuery explain_query;
  explain_query.queue = QOBS_FRONT(inspected.rows).queue;
  ExplainResult explained;
  QOBS_CHECK_STATUS(runtime->explain(explain_query, explained));
  QOBS_CHECK(explained.found);
  QOBS_CHECK(explained.explanation.find("state: pressured") != std::string::npos);

  ExportQuery export_query;
  ExportResult exported;
  QOBS_CHECK_STATUS(runtime->export_data(export_query, exported));
  QOBS_CHECK(exported.rows == 1u);
  QOBS_CHECK(exported.document.find("\"record\":\"queue_state\"") != std::string::npos);

  std::vector<SourceRecord> sources;
  QOBS_CHECK_STATUS(runtime->sources(sources));
  QOBS_CHECK_EQ(sources.size(), 1u);
  QOBS_CHECK(!render_sources_report(sources).empty());

  EventQuery event_query;
  EventResult events;
  QOBS_CHECK_STATUS(runtime->events(event_query, events));
  QOBS_CHECK(events.total_matched > 0u);
  QOBS_CHECK(!render_events_report(events).empty());
  QOBS_CHECK(!render_runtime_status(runtime->status()).empty());

  QOBS_CHECK_STATUS(runtime->stop());
}

QOBS_TEST(runtime, lock_audit_records_no_violation_during_normal_operation) {
  LockAudit::reset();
  auto clock = std::make_shared<ManualClock>(SteadyTime{1000});
  std::unique_ptr<Observatory> runtime = make_runtime(2, clock);
  QOBS_REQUIRE(runtime != nullptr);
  for (std::uint64_t index = 0; index < 8u; ++index) {
    std::uint64_t ticket = 0;
    QOBS_CHECK_STATUS(runtime->submit_document(document_for(index * 2u + 1u, 2, 100), ticket));
  }
  QOBS_CHECK_STATUS(runtime->drain());
  InspectQuery query;
  InspectResult inspected;
  QOBS_CHECK_STATUS(runtime->inspect(query, inspected));
  std::vector<SourceRecord> sources;
  QOBS_CHECK_STATUS(runtime->sources(sources));
  QOBS_CHECK_STATUS(runtime->stop());
  QOBS_CHECK_EQ(LockAudit::total_violations(), 0u);
  LockAudit::reset();
}

QOBS_TEST(runtime, invalid_configuration_is_refused_at_creation) {
  ObservatoryConfig config;
  config.runtime.worker_threads = 4096;
  QOBS_CHECK(!Observatory::create(config, nullptr).has_value());
  config = ObservatoryConfig{};
  config.policy.relative.elevated_permille = 0;
  QOBS_CHECK(!Observatory::create(config, nullptr).has_value());
  config = ObservatoryConfig{};
  config.history.max_samples_per_queue = 0;
  QOBS_CHECK(!Observatory::create(config, nullptr).has_value());
}
