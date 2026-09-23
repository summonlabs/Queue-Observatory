// Queue Observatory benchmarks.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Each case runs a fixed amount of work, counts what completed, and reports the
// count with the elapsed time. The numbers are measurements of finished work,
// not projections.

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "qobs/ingest/Ingest.hpp"
#include "qobs/ingest/Wire.hpp"
#include "qobs/persist/Persistence.hpp"
#include "qobs/policy/Classification.hpp"
#include "qobs/runtime/Observatory.hpp"
#include "qobs/store/Store.hpp"
#include "qobs/store/Window.hpp"

namespace {

class Stopwatch {
 public:
  Stopwatch() : start_(std::chrono::steady_clock::now()) {}

  [[nodiscard]] double seconds() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
  }

 private:
  std::chrono::steady_clock::time_point start_;
};

void report(const char* name, std::uint64_t completed, double seconds, const char* unit) {
  const double rate = seconds > 0.0 ? static_cast<double>(completed) / seconds : 0.0;
  std::printf("%-34s completed=%-10llu %.3fs  %.0f %s/s\n", name,
              static_cast<unsigned long long>(completed), seconds, rate, unit);
}

std::string make_document(std::size_t samples) {
  std::string document;
  for (std::size_t index = 0; index < samples; ++index) {
    qobs::QueueSample sample;
    sample.queue.device = qobs::DeviceId(std::string("leaf-01"));
    sample.queue.port = qobs::PortId(std::string("ethernet1/1"));
    sample.queue.queue = qobs::QueueId::from_raw(static_cast<std::uint32_t>(index % 64u));
    sample.source = qobs::SourceId(std::string("benchmark"));
    sample.incarnation = qobs::IncarnationId(std::string("boot-1"));
    sample.generation = qobs::GenerationId::from_raw(1);
    sample.sequence = qobs::SourceSequence::from_raw(index + 1u);
    sample.authority = qobs::SourceAuthority::Primary;
    sample.observed.ns = static_cast<qobs::Nanos>(1000 + index);
    sample.observed.domain = qobs::ClockDomainId(std::string("device-utc"));
    sample.received = qobs::receive_now();
    sample.set_value(qobs::SampleField::OccupancyCells, (index * 37u) % 1000u);
    sample.set_value(qobs::SampleField::DynamicThresholdCells, 1000u);
    sample.set_value(qobs::SampleField::EnqueuePackets, index + 1u);
    sample.set_value(qobs::SampleField::DropPackets, index / 100u);
    std::string line;
    if (!qobs::encode_sample(sample, line).ok()) {
      return {};
    }
    document += line;
    document.push_back('\n');
  }
  return document;
}

/// The ingest limits the benchmark cases run under. A case that asks for more
/// samples than the default batch limit would measure a truncated document and
/// report a completed count that does not match its own label.
qobs::IngestLimits benchmark_limits(std::size_t samples) {
  qobs::IngestLimits limits;
  limits.max_batch_samples = samples;
  limits.max_batch_queues = samples;
  limits.max_batch_metadata = samples;
  return limits;
}

void benchmark_decode(std::size_t samples) {
  const std::string document = make_document(samples);
  qobs::DecodeOutcome outcome;
  const qobs::Status limits_status = qobs::validate_limits(benchmark_limits(samples));
  if (!limits_status.ok()) {
    std::printf("decode_document                    FAILED: %s\n", limits_status.to_string().c_str());
    return;
  }
  const Stopwatch watch;
  const qobs::Status status =
      qobs::decode_document(document, benchmark_limits(samples), outcome);
  const double seconds = watch.seconds();
  if (!status.ok()) {
    std::printf("decode_document                    FAILED: %s\n", status.to_string().c_str());
    return;
  }
  report("decode_document (samples)", outcome.sample_count, seconds, "sample");
  std::printf("%-34s bytes=%llu\n", "  document size",
              static_cast<unsigned long long>(document.size()));
}

void benchmark_ingest(std::size_t samples) {
  auto clock = std::make_shared<qobs::ManualClock>(qobs::SteadyTime{0});
  qobs::HistoryLimits limits;
  limits.max_queues = 4096;
  limits.max_samples_per_queue = 64;
  limits.max_events_total = 65536;
  limits.max_history_bytes = 512u * 1024u * 1024u;
  qobs::QueueStore store(qobs::PressurePolicy{}, limits, qobs::QueryLimits{}, clock);
  const std::string document = make_document(samples);
  qobs::DecodeOutcome decoded;
  if (!qobs::decode_document(document, benchmark_limits(samples), decoded).ok()) {
    return;
  }
  const qobs::StopToken token;
  qobs::BatchAdmission admission;
  const Stopwatch watch;
  const qobs::Status status = store.ingest_batch(decoded.samples, token, admission);
  const double seconds = watch.seconds();
  if (!status.ok()) {
    std::printf("ingest_batch                       FAILED: %s\n", status.to_string().c_str());
    return;
  }
  report("ingest_batch (samples accepted)", admission.accepted, seconds, "sample");
  std::printf("%-34s rejected=%llu fenced=%llu queues=%llu history_bytes=%llu\n", "  outcome",
              static_cast<unsigned long long>(admission.rejected),
              static_cast<unsigned long long>(admission.fenced),
              static_cast<unsigned long long>(store.queue_count()),
              static_cast<unsigned long long>(store.history_bytes()));
}

void benchmark_classification(std::size_t iterations) {
  const qobs::PressurePolicy& policy = qobs::default_pressure_policy();
  qobs::ClassificationInput input;
  input.queue.device = qobs::DeviceId(std::string("leaf-01"));
  input.queue.port = qobs::PortId(std::string("ethernet1/1"));
  input.queue.queue = qobs::QueueId::from_raw(0);
  input.assessment.freshness = qobs::Freshness::Fresh;
  input.assessment.quality = qobs::EvidenceQuality::Complete;
  input.assessment.provenance = qobs::Provenance::Observed;
  input.assessment.authority = qobs::SourceAuthority::Primary;
  input.reported = qobs::field_bit(qobs::SampleField::OccupancyCells) |
                   qobs::field_bit(qobs::SampleField::DynamicThresholdCells);
  std::uint64_t digest_accumulator = 0;
  std::size_t completed = 0;
  const Stopwatch watch;
  for (std::size_t index = 0; index < iterations; ++index) {
    input.occupancy_cells = index % 1200u;
    input.dynamic_threshold_cells = 1000u;
    input.drops.known = true;
    input.drops.delta = index % 3u;
    const qobs::ClassificationResult result = qobs::classify(policy, input);
    digest_accumulator ^= result.explanation_digest;
    ++completed;
  }
  const double seconds = watch.seconds();
  report("classify (decisions)", completed, seconds, "decision");
  std::printf("%-34s digest_accumulator=%llu\n", "  work check",
              static_cast<unsigned long long>(digest_accumulator));
}

void benchmark_window(std::size_t observations) {
  qobs::WindowSpec spec;
  spec.name = "benchmark";
  spec.duration_ns = 1000000000LL;
  spec.bucket_ns = 10000000LL;
  spec.min_covered_buckets = 1;
  spec.max_buckets = 256;
  qobs::BucketAccumulator accumulator;
  if (!accumulator.configure(spec).ok()) {
    return;
  }
  const Stopwatch watch;
  for (std::size_t index = 0; index < observations; ++index) {
    qobs::WindowObservation observation;
    observation.steady_ns = static_cast<qobs::Nanos>(index * 1000u);
    observation.has_occupancy = true;
    observation.occupancy = index % 1000u;
    observation.drops.known = true;
    observation.drops.delta = index % 5u;
    observation.establishes_current = true;
    accumulator.observe(observation);
  }
  const qobs::WindowAggregate aggregate =
      accumulator.aggregate(static_cast<qobs::Nanos>(observations * 1000u), qobs::FreshnessPolicy{});
  const double seconds = watch.seconds();
  report("window observations folded", aggregate.sample_count, seconds, "observation");
  std::printf("%-34s covered_buckets=%llu max=%llu\n", "  aggregate",
              static_cast<unsigned long long>(aggregate.covered_buckets),
              static_cast<unsigned long long>(aggregate.occupancy_max.value_or(0)));
}

void benchmark_persistence(std::size_t rounds) {
  qobs::DurableSnapshot snapshot;
  snapshot.runtime_id = "qobs-benchmark";
  snapshot.created = qobs::receive_now();
  snapshot.policy_name = "default";
  snapshot.policy_version = 1;
  qobs::DurableQueueState queue;
  queue.queue.device = qobs::DeviceId(std::string("leaf-01"));
  queue.queue.port = qobs::PortId(std::string("ethernet1/1"));
  queue.queue.queue = qobs::QueueId::from_raw(0);
  queue.classes_known = true;
  for (std::size_t index = 0; index < 128u; ++index) {
    qobs::DurableSample sample;
    sample.observed.ns = static_cast<qobs::Nanos>(index);
    sample.observed.domain = qobs::ClockDomainId(std::string("device-utc"));
    sample.received_wall = qobs::WallTime{static_cast<qobs::Nanos>(index)};
    sample.received_steady = qobs::SteadyTime{static_cast<qobs::Nanos>(index)};
    sample.source = qobs::SourceId(std::string("benchmark"));
    sample.generation = qobs::GenerationId::from_raw(1);
    sample.sequence = qobs::SourceSequence::from_raw(index + 1u);
    sample.authority = qobs::SourceAuthority::Primary;
    sample.reported = qobs::field_bit(qobs::SampleField::OccupancyCells);
    sample.values[static_cast<std::size_t>(qobs::SampleField::OccupancyCells)] = index;
    queue.samples.push_back(sample);
  }
  snapshot.queues.push_back(queue);

  std::size_t encoded_rounds = 0;
  std::size_t decoded_rounds = 0;
  std::uint64_t bytes_total = 0;
  const Stopwatch watch;
  for (std::size_t round = 0; round < rounds; ++round) {
    std::vector<std::byte> bytes;
    if (!qobs::encode_snapshot(snapshot, qobs::PersistenceLimits{}, bytes).ok()) {
      break;
    }
    ++encoded_rounds;
    bytes_total += static_cast<std::uint64_t>(bytes.size());
    qobs::DurableSnapshot decoded;
    qobs::DecodeReport report;
    if (!qobs::decode_snapshot(bytes, qobs::PersistenceLimits{}, decoded, report).ok()) {
      break;
    }
    if (!report.clean() || decoded.queues.size() != 1u) {
      break;
    }
    ++decoded_rounds;
  }
  const double seconds = watch.seconds();
  report("persistence encode+decode", decoded_rounds, seconds, "round");
  std::printf("%-34s encoded=%llu bytes=%llu\n", "  work check",
              static_cast<unsigned long long>(encoded_rounds),
              static_cast<unsigned long long>(bytes_total));
}

void benchmark_end_to_end(std::size_t documents, std::size_t samples_per_document,
                          std::size_t workers) {
  qobs::ObservatoryConfig config;
  config.runtime.worker_threads = workers;
  config.runtime.max_pending_batches = 1024;
  config.history.max_queues = 4096;
  config.history.max_samples_per_queue = 64;
  config.history.max_events_total = 65536;
  config.history.max_history_bytes = 512u * 1024u * 1024u;
  auto created = qobs::Observatory::create(config, nullptr);
  if (!created.has_value()) {
    return;
  }
  std::unique_ptr<qobs::Observatory> runtime = std::move(created).value();
  if (!runtime->start().ok()) {
    return;
  }
  const std::string document = make_document(samples_per_document);
  std::size_t accepted = 0;
  const Stopwatch watch;
  for (std::size_t index = 0; index < documents; ++index) {
    std::uint64_t ticket = 0;
    if (!runtime->submit_document(document, ticket).ok()) {
      break;
    }
  }
  (void)runtime->drain();
  const double seconds = watch.seconds();
  const qobs::RuntimeStatus status = runtime->status();
  accepted = status.counters.samples_accepted;
  report("end-to-end documents applied", status.applied_documents, seconds, "document");
  std::printf("%-34s accepted_samples=%llu workers=%llu dropped=%llu\n", "  work check",
              static_cast<unsigned long long>(accepted),
              static_cast<unsigned long long>(workers),
              static_cast<unsigned long long>(status.dropped_documents));
  (void)runtime->stop();
}

}  // namespace

int main() {
  constexpr std::size_t kSamples = 20000;
  std::printf("Queue Observatory benchmarks\n");
  std::printf("samples per document case: %llu\n\n", static_cast<unsigned long long>(kSamples));

  benchmark_decode(kSamples);
  benchmark_ingest(kSamples);
  benchmark_classification(200000);
  benchmark_window(200000);
  benchmark_persistence(200);
  benchmark_end_to_end(64, 256, 1);
  benchmark_end_to_end(64, 256, 4);
  return 0;
}
