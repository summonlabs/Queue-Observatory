// Queue Observatory transport benchmark.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// Measures completed round trips over a real loopback TCP connection between a
// server and a client in this process. The count is of replies actually
// received, not of requests sent.

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "qobs/ingest/Wire.hpp"
#include "qobs/runtime/Observatory.hpp"
#include "qobs/transport/Server.hpp"

namespace {

std::string make_document(std::size_t samples) {
  std::string document;
  for (std::size_t index = 0; index < samples; ++index) {
    qobs::QueueSample sample;
    sample.queue.device = qobs::DeviceId(std::string("leaf-01"));
    sample.queue.port = qobs::PortId(std::string("ethernet1/1"));
    sample.queue.queue = qobs::QueueId::from_raw(0);
    sample.source = qobs::SourceId(std::string("benchmark"));
    sample.incarnation = qobs::IncarnationId(std::string("boot-1"));
    sample.generation = qobs::GenerationId::from_raw(1);
    sample.sequence = qobs::SourceSequence::from_raw(index + 1u);
    sample.authority = qobs::SourceAuthority::Primary;
    sample.observed.ns = static_cast<qobs::Nanos>(index);
    sample.observed.domain = qobs::ClockDomainId(std::string("device-utc"));
    sample.received = qobs::receive_now();
    sample.set_value(qobs::SampleField::OccupancyCells, index % 1000u);
    sample.set_value(qobs::SampleField::DynamicThresholdCells, 1000u);
    std::string line;
    if (!qobs::encode_sample(sample, line).ok()) {
      return {};
    }
    document += line;
    document.push_back('\n');
  }
  return document;
}

}  // namespace

int main() {
  qobs::ObservatoryConfig config;
  config.runtime.worker_threads = 0;
  config.transport.max_frame_bytes = 4u * 1024u * 1024u;
  auto created = qobs::Observatory::create(config, nullptr);
  if (!created.has_value()) {
    return 1;
  }
  std::unique_ptr<qobs::Observatory> runtime = std::move(created).value();
  if (!runtime->start().ok()) {
    return 1;
  }
  qobs::TransportServer server(*runtime, config.transport);
  if (!server.listen_on("127.0.0.1", 0, 8).ok()) {
    return 1;
  }
  qobs::StopSource stop;
  std::thread serving([&] { (void)server.serve(stop.token()); });

  const std::string document = make_document(64);
  qobs::TransportClient client;
  if (!client.connect_to("127.0.0.1", server.local_port(), "benchmark").ok()) {
    stop.request_stop();
    serving.join();
    return 1;
  }

  constexpr std::size_t kRoundTrips = 200;
  std::size_t completed = 0;
  std::size_t accepted = 0;
  const auto start = std::chrono::steady_clock::now();
  for (std::size_t index = 0; index < kRoundTrips; ++index) {
    qobs::BatchReply reply;
    if (!client.send_batch(document, reply).ok()) {
      break;
    }
    ++completed;
    accepted += reply.report.admission.accepted;
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::printf("transport round trips completed=%llu %.3fs %.0f round-trip/s\n",
              static_cast<unsigned long long>(completed), seconds,
              seconds > 0.0 ? static_cast<double>(completed) / seconds : 0.0);
  std::printf("samples accepted over the wire=%llu documents=%llu\n",
              static_cast<unsigned long long>(accepted), static_cast<unsigned long long>(completed));
  (void)client.close();
  stop.request_stop();
  serving.join();
  (void)runtime->stop();
  return completed == kRoundTrips ? 0 : 1;
}
