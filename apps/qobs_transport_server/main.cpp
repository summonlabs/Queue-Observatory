// Queue Observatory transport server.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// This process is a real, independent operating-system process that accepts the
// canonical observation document over TCP. It exposes no configuration path of
// any kind.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "qobs/core/Cancellation.hpp"
#include "qobs/core/Text.hpp"
#include "qobs/runtime/Observatory.hpp"
#include "qobs/runtime/Report.hpp"
#include "qobs/transport/Server.hpp"
#include "qobs/transport/Socket.hpp"

namespace {

struct Arguments {
  std::string host{"127.0.0.1"};
  std::uint16_t port{0};
  bool port_given{false};
  std::uintptr_t listen_handle{0};
  bool handle_given{false};
  std::string port_file{};
  std::size_t workers{0};
  std::size_t max_connections{32};
  std::size_t exit_after_batches{0};
  std::string persist_dir{};
};

bool parse(int argc, char** argv, Arguments& arguments) {
  for (int index = 1; index < argc; ++index) {
    const std::string token = argv[index];
    const auto next = [&](std::string& out) -> bool {
      if (index + 1 >= argc) {
        return false;
      }
      out = argv[++index];
      return true;
    };
    if (token == "--host") {
      if (!next(arguments.host)) {
        return false;
      }
    } else if (token == "--port") {
      std::string text;
      if (!next(text)) {
        return false;
      }
      std::uint64_t value = 0;
      if (!qobs::text::parse_u64(text, value) || value > 65535u) {
        return false;
      }
      arguments.port = static_cast<std::uint16_t>(value);
      arguments.port_given = true;
    } else if (token == "--listen-handle" || token == "--listen-fd") {
      std::string text;
      if (!next(text)) {
        return false;
      }
      std::uint64_t value = 0;
      if (!qobs::text::parse_u64(text, value)) {
        return false;
      }
      arguments.listen_handle = static_cast<std::uintptr_t>(value);
      arguments.handle_given = true;
    } else if (token == "--port-file") {
      if (!next(arguments.port_file)) {
        return false;
      }
    } else if (token == "--workers") {
      std::string text;
      if (!next(text)) {
        return false;
      }
      std::uint64_t value = 0;
      if (!qobs::text::parse_u64(text, value)) {
        return false;
      }
      arguments.workers = static_cast<std::size_t>(value);
    } else if (token == "--max-connections") {
      std::string text;
      if (!next(text)) {
        return false;
      }
      std::uint64_t value = 0;
      if (!qobs::text::parse_u64(text, value) || value == 0u) {
        return false;
      }
      arguments.max_connections = static_cast<std::size_t>(value);
    } else if (token == "--exit-after-batches") {
      std::string text;
      if (!next(text)) {
        return false;
      }
      std::uint64_t value = 0;
      if (!qobs::text::parse_u64(text, value)) {
        return false;
      }
      arguments.exit_after_batches = static_cast<std::size_t>(value);
    } else if (token == "--persist-dir") {
      if (!next(arguments.persist_dir)) {
        return false;
      }
    } else {
      std::fprintf(stderr, "qobs-transport-server: unknown option '%s'\n", token.c_str());
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Arguments arguments;
  if (!parse(argc, argv, arguments)) {
    std::fputs(
        "usage: qobs-transport-server [--host H] [--port N] [--listen-handle H] [--port-file P]\n"
        "                             [--workers N] [--max-connections N]\n"
        "                             [--exit-after-batches N] [--persist-dir P]\n",
        stderr);
    return 2;
  }

  qobs::ObservatoryConfig config;
  config.runtime.worker_threads = arguments.workers;
  config.transport.max_connections = arguments.max_connections;
  if (!arguments.persist_dir.empty()) {
    config.persistence_directory = std::filesystem::path(arguments.persist_dir);
    config.recover_on_start = true;
  }
  auto runtime = qobs::Observatory::create(config, nullptr);
  if (!runtime.has_value()) {
    std::fprintf(stderr, "qobs-transport-server: %s\n", qobs::Status(runtime.error()).to_string().c_str());
    return 1;
  }
  std::unique_ptr<qobs::Observatory> observatory = std::move(runtime).value();
  if (const qobs::Status started = observatory->start(); !started.ok()) {
    std::fprintf(stderr, "qobs-transport-server: %s\n", started.to_string().c_str());
    return 1;
  }

  qobs::TransportServer server(*observatory, config.transport);
  if (arguments.handle_given) {
    qobs::Socket adopted = qobs::Socket::adopt(arguments.listen_handle);
    const qobs::Status status = server.adopt(std::move(adopted));
    if (!status.ok()) {
      std::fprintf(stderr, "qobs-transport-server: %s\n", status.to_string().c_str());
      return 1;
    }
  } else {
    const qobs::Status status = server.listen_on(arguments.host, arguments.port, 64);
    if (!status.ok()) {
      std::fprintf(stderr, "qobs-transport-server: %s\n", status.to_string().c_str());
      return 1;
    }
  }
  const std::uint16_t bound = server.local_port();
  std::printf("listening host=%s port=%u runtime=%s\n", arguments.host.c_str(),
              static_cast<unsigned>(bound),
              observatory->store().session().runtime_id.c_str());
  std::fflush(stdout);

  if (!arguments.port_file.empty()) {
    std::FILE* file = std::fopen(arguments.port_file.c_str(), "wb");
    if (file == nullptr) {
      std::fprintf(stderr, "qobs-transport-server: cannot write the port file\n");
      return 1;
    }
    std::fprintf(file, "%u\n", static_cast<unsigned>(bound));
    std::fclose(file);
  }

  server.set_stop_after_batches(arguments.exit_after_batches);
  qobs::StopSource stop;
  const qobs::Status served = server.serve(stop.token());
  const qobs::TransportServerStats stats = server.stats();
  std::printf("connections=%llu frames=%llu batches=%llu rejected_frames=%llu failures=%llu\n",
              static_cast<unsigned long long>(stats.connections_accepted),
              static_cast<unsigned long long>(stats.frames_received),
              static_cast<unsigned long long>(stats.batches_applied),
              static_cast<unsigned long long>(stats.frames_rejected),
              static_cast<unsigned long long>(stats.batch_failures));
  std::fflush(stdout);
  const qobs::Status stopped = observatory->stop();
  if (!stopped.ok()) {
    std::fprintf(stderr, "qobs-transport-server: %s\n", stopped.to_string().c_str());
    return 1;
  }
  if (!served.ok()) {
    std::fprintf(stderr, "qobs-transport-server: %s\n", served.to_string().c_str());
    return 1;
  }
  return arguments.exit_after_batches > 0u &&
                 stats.batches_applied < arguments.exit_after_batches
             ? 5
             : 0;
}
