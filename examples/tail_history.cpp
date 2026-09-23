// Example: read a bounded window of retained history.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <memory>
#include <string>

#include "qobs/ingest/Wire.hpp"
#include "qobs/runtime/Observatory.hpp"
#include "qobs/runtime/Report.hpp"

int main() {
  qobs::ObservatoryConfig config;
  config.runtime.worker_threads = 0;
  config.history.max_samples_per_queue = 64;
  auto created = qobs::Observatory::create(config, nullptr);
  if (!created.has_value()) {
    return 1;
  }
  std::unique_ptr<qobs::Observatory> runtime = std::move(created).value();
  if (!runtime->start().ok()) {
    return 1;
  }

  std::string document;
  for (std::uint64_t index = 0; index < 40u; ++index) {
    qobs::QueueSample sample;
    sample.queue.device = qobs::DeviceId(std::string("leaf-01"));
    sample.queue.port = qobs::PortId(std::string("ethernet1/1"));
    sample.queue.queue = qobs::QueueId::from_raw(0);
    sample.source = qobs::SourceId(std::string("telemetry-a"));
    sample.incarnation = qobs::IncarnationId(std::string("boot-7"));
    sample.generation = qobs::GenerationId::from_raw(1);
    sample.sequence = qobs::SourceSequence::from_raw(index + 1u);
    sample.authority = qobs::SourceAuthority::Primary;
    // The clock domain of the observation travels with the sample. Receive time
    // is not set here: the runtime stamps it when the evidence arrives, which is
    // the only way it can be trusted.
    sample.observed.ns = static_cast<qobs::Nanos>(index) * 1000000;
    sample.observed.domain = qobs::ClockDomainId(std::string("device-utc"));
    // A deterministic sawtooth so the excursion is visible in the report.
    const std::uint64_t occupancy = (index % 10u) * 90u;
    sample.set_value(qobs::SampleField::OccupancyCells, occupancy);
    sample.set_value(qobs::SampleField::DynamicThresholdCells, 1000u);
    sample.set_value(qobs::SampleField::EnqueuePackets, index + 1u);
    std::string line;
    if (!qobs::encode_sample(sample, line).ok()) {
      return 1;
    }
    document += line;
    document.push_back('\n');
  }

  qobs::IngestReport report;
  if (!runtime->ingest_document(document, report).ok()) {
    return 1;
  }

  qobs::HistoryQuery query;
  query.queue.device = qobs::DeviceId(std::string("leaf-01"));
  query.queue.port = qobs::PortId(std::string("ethernet1/1"));
  query.queue.queue = qobs::QueueId::from_raw(0);
  query.window.name = "1s";
  query.window.duration_ns = 1000000000LL;
  query.window.bucket_ns = 125000000LL;
  query.window.min_covered_buckets = 1;
  query.window.max_buckets = 8;
  query.page.limit = 8;
  qobs::HistoryResult history;
  (void)runtime->history(query, history);
  std::fputs(qobs::render_history_report(history).c_str(), stdout);

  qobs::MicroburstQuery burst_query;
  qobs::MicroburstResult bursts;
  (void)runtime->microburst(burst_query, bursts);
  std::fputs(qobs::render_microburst_report(bursts).c_str(), stdout);

  (void)runtime->stop();
  return 0;
}
