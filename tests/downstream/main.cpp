// Independent downstream consumer of the installed Queue Observatory package.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// This program links only against the exported targets of an installed package.
// It exercises one complete observation path and reports the results, so a
// broken install, a missing header, or a missing transitive dependency shows up
// as a build or run failure here rather than in a downstream user's project.

#include <cstdio>
#include <memory>
#include <string>

#include <qobs/ingest/Wire.hpp>
#include <qobs/persist/Format.hpp>
#include <qobs/runtime/Observatory.hpp>
#include <qobs/runtime/Report.hpp>
#include <qobs/transport/Protocol.hpp>
#include <qobs/version.hpp>

int main() {
  std::printf("queue-observatory version %s\n", std::string(qobs::version_string()).c_str());
  std::printf("persistence format %u, wire format %u, policy %u\n",
              QOBS_PERSISTENCE_FORMAT_VERSION, QOBS_WIRE_FORMAT_VERSION, QOBS_POLICY_VERSION);

  qobs::ObservatoryConfig config;
  config.runtime.worker_threads = 0;
  auto created = qobs::Observatory::create(config, nullptr);
  if (!created.has_value()) {
    std::fprintf(stderr, "create failed: %s\n", created.error().to_string().c_str());
    return 1;
  }
  std::unique_ptr<qobs::Observatory> runtime = std::move(created).value();
  if (const qobs::Status started = runtime->start(); !started.ok()) {
    std::fprintf(stderr, "start failed: %s\n", started.to_string().c_str());
    return 1;
  }

  std::string document;
  for (std::uint64_t index = 0; index < 3u; ++index) {
    qobs::QueueSample sample;
    sample.queue.device = qobs::DeviceId(std::string("leaf-01"));
    sample.queue.port = qobs::PortId(std::string("ethernet1/1"));
    sample.queue.queue = qobs::QueueId::from_raw(0);
    sample.source = qobs::SourceId(std::string("downstream"));
    sample.incarnation = qobs::IncarnationId(std::string("boot-1"));
    sample.generation = qobs::GenerationId::from_raw(1);
    sample.sequence = qobs::SourceSequence::from_raw(index + 1u);
    sample.authority = qobs::SourceAuthority::Primary;
    sample.observed.ns = static_cast<qobs::Nanos>(1000 + index);
    sample.observed.domain = qobs::ClockDomainId(std::string("device-utc"));
    sample.set_value(qobs::SampleField::OccupancyCells, 700u);
    sample.set_value(qobs::SampleField::DynamicThresholdCells, 1000u);
    sample.set_value(qobs::SampleField::EnqueuePackets, index + 1u);
    std::string line;
    if (const qobs::Status encoded = qobs::encode_sample(sample, line); !encoded.ok()) {
      std::fprintf(stderr, "encode failed: %s\n", encoded.to_string().c_str());
      return 1;
    }
    document += line;
    document.push_back('\n');
  }

  qobs::IngestReport report;
  if (const qobs::Status ingested = runtime->ingest_document(document, report); !ingested.ok()) {
    std::fprintf(stderr, "ingest failed: %s\n", ingested.to_string().c_str());
    return 1;
  }
  std::printf("accepted=%zu rejected=%zu\n", report.admission.accepted, report.admission.rejected);
  if (report.admission.accepted != 3u) {
    std::fputs("the runtime did not accept every sample\n", stderr);
    return 1;
  }

  qobs::PressureQuery query;
  qobs::PressureResult pressure;
  if (const qobs::Status queried = runtime->pressure(query, pressure); !queried.ok()) {
    std::fprintf(stderr, "pressure query failed: %s\n", queried.to_string().c_str());
    return 1;
  }
  if (pressure.rows.size() != 1u) {
    std::fputs("the runtime did not report exactly one queue\n", stderr);
    return 1;
  }
  std::printf("state=%s freshness=%s\n",
              std::string(qobs::to_string(pressure.rows.front().state)).c_str(),
              std::string(qobs::to_string(pressure.rows.front().assessment.freshness)).c_str());
  // Three samples at 700 of a 1000 cell threshold: above the elevated band
  // (500) and below the pressure band (750).
  const bool live = pressure.rows.front().state == qobs::PressureState::Elevated;
  if (!live) {
    std::fputs("expected the queue to be reported as elevated\n", stderr);
    return 1;
  }

  qobs::ExportQuery export_query;
  qobs::ExportResult exported;
  if (const qobs::Status exported_status = runtime->export_data(export_query, exported);
      !exported_status.ok()) {
    std::fprintf(stderr, "export failed: %s\n", exported_status.to_string().c_str());
    return 1;
  }
  std::printf("export rows=%zu bytes=%zu\n", exported.rows, exported.document.size());

  qobs::DurableSnapshot snapshot;
  if (const qobs::Status built = qobs::build_snapshot(runtime->store(), 16u, 16u, snapshot);
      !built.ok()) {
    std::fprintf(stderr, "snapshot failed: %s\n", built.to_string().c_str());
    return 1;
  }
  std::vector<std::byte> encoded;
  if (const qobs::Status persisted = qobs::encode_snapshot(snapshot, qobs::PersistenceLimits{},
                                                           encoded);
      !persisted.ok()) {
    std::fprintf(stderr, "encode snapshot failed: %s\n", persisted.to_string().c_str());
    return 1;
  }
  std::printf("snapshot queues=%zu bytes=%zu\n", snapshot.queues.size(), encoded.size());

  const qobs::transport::FrameType frame_type = qobs::transport::FrameType::Batch;
  std::printf("transport frame kind=%s\n",
              std::string(qobs::transport::to_string(frame_type)).c_str());

  if (const qobs::Status stopped = runtime->stop(); !stopped.ok()) {
    std::fprintf(stderr, "stop failed: %s\n", stopped.to_string().c_str());
    return 1;
  }
  std::puts("downstream consumer ok");
  return 0;
}
