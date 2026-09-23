#include "qobs/runtime/Observatory.hpp"

#include "qobs/core/LockAudit.hpp"
#include "qobs/core/Text.hpp"
#include "qobs/ingest/Wire.hpp"
#include "qobs/policy/Explanation.hpp"

namespace qobs {

Status apply_recovered_snapshot(QueueStore& store, const DurableSnapshot& snapshot,
                                const DecodeReport& report, RecoverySummary& summary) {
  summary.recovered = true;
  summary.report = report;
  summary.queues = snapshot.queues.size();
  summary.sources = snapshot.sources.size();
  summary.events = snapshot.events.size();
  summary.evidence_marked_stale = true;

  if (snapshot.has_metadata) {
    MetadataAdmission admission;
    QOBS_TRY(store.apply_metadata(snapshot.metadata, admission));
    summary.metadata_present = true;
  }

  // Recovered samples are re-admitted in the order they were persisted. Their
  // receive times belonged to a previous process, so they are reset to this
  // process's steady origin: a value that is guaranteed to be unrelated to the
  // new clock and therefore must not be compared with it. The store caps the
  // resulting freshness as well, so two independent mechanisms keep recovered
  // evidence from ever being reported as current.
  std::vector<QueueSample> samples;
  std::size_t total = 0;
  for (const DurableQueueState& queue : snapshot.queues) {
    total += queue.samples.size();
  }
  samples.reserve(total);
  for (const DurableQueueState& queue : snapshot.queues) {
    for (const DurableSample& durable : queue.samples) {
      QueueSample sample;
      sample.queue = queue.queue;
      sample.classes = queue.classes;
      sample.source = durable.source;
      sample.incarnation = durable.incarnation;
      sample.generation = durable.generation;
      sample.sequence = durable.sequence;
      sample.authority = durable.authority;
      sample.observed = durable.observed;
      sample.received.steady = SteadyTime{0};
      sample.received.wall = durable.received_wall;
      sample.reported = durable.reported;
      sample.declared = durable.reported;
      sample.declared_version = wire::kVersion;
      for (std::size_t index = 0; index < kSampleFieldCount; ++index) {
        if (has_field(durable.reported, static_cast<SampleField>(index))) {
          sample.set_value(static_cast<SampleField>(index), durable.values[index]);
        }
      }
      samples.push_back(std::move(sample));
    }
  }

  summary.samples_presented = samples.size();
  BatchAdmission admission;
  QOBS_TRY(store.admit_recovered(samples, admission));
  summary.samples = admission.accepted;
  summary.samples_rejected = admission.rejected;
  summary.samples_fenced = admission.fenced;

  std::size_t imported = 0;
  QOBS_TRY(store.import_events(snapshot.events, imported));
  summary.events = imported;

  summary.diagnostic =
      report.clean() ? "recovered a clean segment" : "recovered a damaged segment: " +
                                                         report.diagnostic;
  return Status::success();
}

}  // namespace qobs
