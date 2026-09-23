// Example: observe one queue and explain the resulting state.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "qobs/ingest/Wire.hpp"
#include "qobs/runtime/Observatory.hpp"
#include "qobs/runtime/Report.hpp"

int main() {
  qobs::ObservatoryConfig config;
  config.runtime.worker_threads = 0;
  auto created = qobs::Observatory::create(config, nullptr);
  if (!created.has_value()) {
    std::fprintf(stderr, "cannot create the runtime: %s\n", created.error().to_string().c_str());
    return 1;
  }
  std::unique_ptr<qobs::Observatory> runtime = std::move(created).value();
  if (const qobs::Status started = runtime->start(); !started.ok()) {
    std::fprintf(stderr, "cannot start the runtime: %s\n", started.to_string().c_str());
    return 1;
  }

  // Build three observations of the same queue. Occupancy climbs into the
  // pressure band and then a drop appears.
  std::string document;
  const std::vector<std::uint64_t> occupancy{120u, 820u, 980u};
  const std::vector<std::uint64_t> drops{0u, 0u, 4u};
  for (std::size_t index = 0; index < occupancy.size(); ++index) {
    qobs::QueueSample sample;
    sample.queue.device = qobs::DeviceId(std::string("leaf-01"));
    sample.queue.port = qobs::PortId(std::string("ethernet1/1"));
    sample.queue.queue = qobs::QueueId::from_raw(3);
    sample.source = qobs::SourceId(std::string("telemetry-a"));
    sample.incarnation = qobs::IncarnationId(std::string("boot-7"));
    sample.generation = qobs::GenerationId::from_raw(12);
    sample.sequence = qobs::SourceSequence::from_raw(index + 1u);
    sample.authority = qobs::SourceAuthority::Primary;
    sample.observed.ns = static_cast<qobs::Nanos>(1000 + index * 1000);
    sample.observed.domain = qobs::ClockDomainId(std::string("device-utc"));
    sample.received = qobs::receive_now();
    sample.set_value(qobs::SampleField::OccupancyCells, occupancy[index]);
    sample.set_value(qobs::SampleField::DynamicThresholdCells, 1000u);
    sample.set_value(qobs::SampleField::EnqueuePackets, (index + 1u) * 10u);
    sample.set_value(qobs::SampleField::DropPackets, drops[index]);
    std::string line;
    if (const qobs::Status encoded = qobs::encode_sample(sample, line); !encoded.ok()) {
      std::fprintf(stderr, "cannot encode: %s\n", encoded.to_string().c_str());
      return 1;
    }
    document += line;
    document.push_back('\n');
  }

  qobs::IngestReport report;
  if (const qobs::Status status = runtime->ingest_document(document, report); !status.ok()) {
    std::fprintf(stderr, "cannot ingest: %s\n", status.to_string().c_str());
    return 1;
  }
  std::fputs(qobs::render_ingest_report(report).c_str(), stdout);

  qobs::InspectQuery inspect_query;
  qobs::InspectResult inspected;
  (void)runtime->inspect(inspect_query, inspected);
  std::fputs(qobs::render_inspect_table(inspected, true).c_str(), stdout);

  qobs::ExplainQuery explain_query;
  explain_query.queue = inspected.rows.front().queue;
  qobs::ExplainResult explained;
  (void)runtime->explain(explain_query, explained);
  std::fputs(explained.explanation.c_str(), stdout);

  (void)runtime->stop();
  return 0;
}
