#include "support/TestHarness.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "qobs/core/LockAudit.hpp"
#include "qobs/ingest/Wire.hpp"
#include "qobs/runtime/Observatory.hpp"
#include "qobs/store/Store.hpp"

using namespace qobs;
using qobs::test::SampleBuilder;

namespace {

std::string document_for(const char* source, const char* incarnation, std::uint64_t first_sequence,
                         std::size_t count, std::uint32_t queue_index) {
  std::string document;
  for (std::size_t index = 0; index < count; ++index) {
    SampleBuilder builder("leaf-01", "ethernet1/1", queue_index, source, incarnation);
    const Nanos at = 1000 + static_cast<Nanos>(first_sequence + index) * 10;
    builder.sequence(first_sequence + index);
    builder.received_at(at);
    builder.observed_at(at);
    builder.set(SampleField::OccupancyCells, 100u + index);
    builder.set(SampleField::DynamicThresholdCells, 1000u);
    builder.set(SampleField::EnqueuePackets, first_sequence + index);
    std::string line;
    const Status status = encode_sample(builder.build(), line);
    (void)status;
    document += line;
    document.push_back('\n');
  }
  return document;
}

std::unique_ptr<Observatory> make_runtime(std::size_t workers) {
  ObservatoryConfig config;
  config.runtime.worker_threads = workers;
  config.runtime.max_pending_batches = 4096;
  config.history.max_queues = 64;
  config.history.max_samples_per_queue = 128;
  config.history.max_events_total = 65536;
  auto created = Observatory::create(config, nullptr);
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

QOBS_TEST(concurrency, many_submitters_are_all_accounted_for) {
  std::unique_ptr<Observatory> runtime = make_runtime(4);
  QOBS_REQUIRE(runtime != nullptr);

  constexpr std::size_t kThreads = 8;
  constexpr std::size_t kDocumentsPerThread = 24;
  constexpr std::size_t kSamplesPerDocument = 8;

  std::atomic<std::size_t> submitted{0};
  std::atomic<std::size_t> rejected{0};
  std::vector<std::thread> submitters;
  submitters.reserve(kThreads);
  for (std::size_t thread_index = 0; thread_index < kThreads; ++thread_index) {
    submitters.emplace_back([&, thread_index] {
      const std::string source = "collector-" + std::to_string(thread_index);
      for (std::size_t document_index = 0; document_index < kDocumentsPerThread;
           ++document_index) {
        const std::uint64_t first_sequence = document_index * kSamplesPerDocument + 1u;
        const std::string document = document_for(source.c_str(), "boot-1", first_sequence,
                                                  kSamplesPerDocument,
                                                  static_cast<std::uint32_t>(thread_index));
        // Retry while the bounded queue is full. Busy is a normal, documented
        // answer, not a failure.
        for (;;) {
          std::uint64_t ticket = 0;
          const Status status = runtime->submit_document(document, ticket);
          if (status.ok()) {
            submitted.fetch_add(1);
            break;
          }
          if (status.code() != ErrorCode::Busy) {
            rejected.fetch_add(1);
            break;
          }
          std::this_thread::yield();
        }
      }
    });
  }
  for (std::thread& thread : submitters) {
    thread.join();
  }
  QOBS_CHECK_EQ(rejected.load(), 0u);
  QOBS_CHECK_STATUS(runtime->drain());

  const RuntimeStatus status = runtime->status();
  QOBS_CHECK_EQ(status.applied_documents, submitted.load());
  QOBS_CHECK_EQ(status.queued_documents, 0u);
  QOBS_CHECK_EQ(status.counters.samples_accepted, submitted.load() * kSamplesPerDocument);
  QOBS_CHECK_EQ(status.counters.samples_fenced, 0u);
  QOBS_CHECK_EQ(status.queues, kThreads);
  QOBS_CHECK_STATUS(runtime->stop());
}

QOBS_TEST(concurrency, readers_observe_consistent_snapshots_while_writing) {
  std::unique_ptr<Observatory> runtime = make_runtime(2);
  QOBS_REQUIRE(runtime != nullptr);

  constexpr std::size_t kDocuments = 40;
  constexpr std::size_t kSamplesPerDocument = 4;
  std::atomic<bool> writing{true};
  std::atomic<std::size_t> observations{0};
  std::vector<std::thread> readers;
  for (std::size_t index = 0; index < 3u; ++index) {
    readers.emplace_back([&] {
      InspectQuery inspect_query;
      PressureQuery pressure_query;
      EventQuery event_query;
      event_query.page.limit = 64;
      while (writing.load()) {
        InspectResult inspected;
        PressureResult pressure;
        EventResult events;
        if (runtime->inspect(inspect_query, inspected).ok()) {
          // A row must never claim more retained records than its capacity.
          for (const InspectRow& row : inspected.rows) {
            if (row.history_records > row.history_capacity) {
              QOBS_CHECK(false);
            }
          }
        }
        (void)runtime->pressure(pressure_query, pressure);
        (void)runtime->events(event_query, events);
        observations.fetch_add(1);
      }
    });
  }

  for (std::size_t document_index = 0; document_index < kDocuments; ++document_index) {
    std::uint64_t ticket = 0;
    const Status status = runtime->submit_document(
        document_for("collector-0", "boot-1", document_index * kSamplesPerDocument + 1u,
                     kSamplesPerDocument, 0u),
        ticket);
    QOBS_CHECK_STATUS(status);
  }
  QOBS_CHECK_STATUS(runtime->drain());
  writing.store(false);
  for (std::thread& reader : readers) {
    reader.join();
  }
  QOBS_CHECK(observations.load() > 0u);
  QOBS_CHECK_EQ(runtime->status().counters.samples_accepted, kDocuments * kSamplesPerDocument);
  QOBS_CHECK_STATUS(runtime->stop());
}

QOBS_TEST(concurrency, stop_cancels_workers_and_is_idempotent) {
  std::unique_ptr<Observatory> runtime = make_runtime(4);
  QOBS_REQUIRE(runtime != nullptr);
  std::vector<std::thread> submitters;
  std::atomic<bool> keep_going{true};
  for (std::size_t index = 0; index < 4u; ++index) {
    submitters.emplace_back([&, index] {
      std::size_t sequence = 1;
      while (keep_going.load()) {
        std::uint64_t ticket = 0;
        const Status status = runtime->submit_document(
            document_for(("collector-" + std::to_string(index)).c_str(), "boot-1", sequence, 4,
                         static_cast<std::uint32_t>(index)),
            ticket);
        sequence += 4;
        if (!status.ok() && status.code() != ErrorCode::Busy) {
          break;
        }
        std::this_thread::yield();
      }
    });
  }
  QOBS_CHECK_STATUS(runtime->stop());
  keep_going.store(false);
  for (std::thread& thread : submitters) {
    thread.join();
  }
  const RuntimeStatus status = runtime->status();
  QOBS_CHECK_EQ(status.state, RuntimeState::Stopped);
  QOBS_CHECK_EQ(status.queued_documents, 0u);
  QOBS_CHECK_STATUS(runtime->stop());
  std::uint64_t ticket = 0;
  QOBS_CHECK_EQ(runtime->submit_document(document_for("x", "y", 1, 1, 0), ticket).code(),
                ErrorCode::NotRunning);
}

QOBS_TEST(concurrency, store_serves_parallel_readers_without_a_lock_violation) {
  LockAudit::reset();
  auto clock = std::make_shared<ManualClock>(SteadyTime{0});
  HistoryLimits limits;
  limits.max_queues = 16;
  limits.max_samples_per_queue = 64;
  limits.max_events_total = 4096;
  QueueStore store(PressurePolicy{}, limits, QueryLimits{}, clock);

  std::atomic<bool> stop_writing{false};
  std::thread writer([&] {
    std::uint64_t sequence = 0;
    while (!stop_writing.load()) {
      ++sequence;
      SampleBuilder builder("leaf-01", "ethernet1/1",
                            static_cast<std::uint32_t>(sequence % 8u), "collector-a", "boot-1");
      const Nanos at = 1000 + static_cast<Nanos>(sequence);
      builder.sequence(sequence).received_at(at).observed_at(at);
      builder.set(SampleField::OccupancyCells, sequence % 1000u);
      builder.set(SampleField::DynamicThresholdCells, 1000u);
      AdmissionResult result;
      (void)store.ingest(builder.build(), result);
    }
  });

  std::vector<std::thread> readers;
  for (std::size_t index = 0; index < 4u; ++index) {
    readers.emplace_back([&] {
      for (int iteration = 0; iteration < 200; ++iteration) {
        InspectQuery inspect_query;
        InspectResult inspected;
        (void)store.inspect(inspect_query, inspected);
        PressureQuery pressure_query;
        PressureResult pressure;
        (void)store.pressure(pressure_query, pressure);
        std::vector<SourceRecord> sources;
        (void)store.sources(sources);
      }
    });
  }
  for (std::thread& reader : readers) {
    reader.join();
  }
  stop_writing.store(true);
  writer.join();
  QOBS_CHECK_EQ(LockAudit::total_violations(), 0u);
  LockAudit::reset();
}

QOBS_TEST(concurrency, concurrent_metadata_revisions_leave_exactly_one_current_table) {
  auto clock = std::make_shared<ManualClock>(SteadyTime{0});
  HistoryLimits limits;
  limits.max_queues = 8;
  QueueStore store(PressurePolicy{}, limits, QueryLimits{}, clock);

  std::atomic<std::size_t> accepted{0};
  std::vector<std::thread> threads;
  for (std::size_t index = 0; index < 4u; ++index) {
    threads.emplace_back([&, index] {
      for (std::uint64_t revision = 1; revision <= 16u; ++revision) {
        ClassMetadataTable table;
        table.revision = Revision::from_raw(index * 100u + revision);
        table.generation = GenerationId::from_raw(1);
        table.source = SourceId(std::string("cmdb"));
        MetadataAdmission admission;
        if (store.apply_metadata(table, admission).ok() && admission.accepted) {
          accepted.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  QOBS_CHECK(accepted.load() >= 1u);
  ClassMetadataTable table;
  bool has_metadata = false;
  QOBS_CHECK_STATUS(store.metadata_snapshot(table, has_metadata));
  QOBS_CHECK(has_metadata);
  QOBS_CHECK(table.revision.valid());
  QOBS_CHECK_EQ(store.counters().metadata_updates, accepted.load());
}
