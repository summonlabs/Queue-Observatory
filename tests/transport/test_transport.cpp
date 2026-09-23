#include "support/TestHarness.hpp"

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "qobs/ingest/Wire.hpp"
#include "qobs/runtime/Observatory.hpp"
#include "qobs/transport/Server.hpp"
#include "qobs/transport/Socket.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

using namespace qobs;
using qobs::test::SampleBuilder;

namespace {

#ifndef QOBS_TRANSPORT_SERVER_EXE
#define QOBS_TRANSPORT_SERVER_EXE "qobs-transport-server"
#endif
#ifndef QOBS_TRANSPORT_CLIENT_EXE
#define QOBS_TRANSPORT_CLIENT_EXE "qobs-transport-client"
#endif

/// A real, independent operating-system process started by this test.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess() {
    if (started_) {
      wait();
    }
  }

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  [[nodiscard]] bool started() const noexcept { return started_; }

  bool spawn(const std::string& executable, const std::vector<std::string>& arguments,
             bool inherit_handles) {
    std::string command = "\"" + executable + "\"";
    for (const std::string& argument : arguments) {
      command += " \"";
      command += argument;
      command += "\"";
    }
#ifdef _WIN32
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');
    PROCESS_INFORMATION info{};
    const BOOL ok = CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr,
                                   inherit_handles ? TRUE : FALSE, 0, nullptr, nullptr, &startup,
                                   &info);
    if (ok == FALSE) {
      return false;
    }
    info_ = info;
    CloseHandle(info_.hThread);
    started_ = true;
    return true;
#else
    (void)inherit_handles;
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.c_str()));
    for (const std::string& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int rc = ::posix_spawn(&pid, executable.c_str(), nullptr, nullptr, argv.data(), environ);
    if (rc != 0) {
      return false;
    }
    pid_ = pid;
    started_ = true;
    return true;
#endif
  }

  int wait() {
    if (!started_) {
      return -1;
    }
    started_ = false;
#ifdef _WIN32
    WaitForSingleObject(info_.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(info_.hProcess, &exit_code);
    CloseHandle(info_.hProcess);
    return static_cast<int>(exit_code);
#else
    int status = 0;
    ::waitpid(pid_, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
  }

 private:
#ifdef _WIN32
  PROCESS_INFORMATION info_{};
#else
  pid_t pid_{0};
#endif
  bool started_{false};
};

std::string transport_document(std::uint64_t first_sequence, std::size_t count) {
  std::string document;
  for (std::size_t index = 0; index < count; ++index) {
    SampleBuilder builder("leaf-01", "ethernet1/1", 0u, "collector-net", "boot-net");
    const Nanos at = 1000 + static_cast<Nanos>(index) * 1000;
    builder.sequence(first_sequence + index);
    builder.received_at(at);
    builder.observed_at(at);
    builder.set(SampleField::OccupancyCells, 100u + index);
    builder.set(SampleField::DynamicThresholdCells, 1000u);
    builder.set(SampleField::EnqueuePackets, index + 1u);
    std::string line;
    const Status status = encode_sample(builder.build(), line);
    (void)status;
    document += line;
    document.push_back('\n');
  }
  return document;
}

std::unique_ptr<Observatory> make_runtime() {
  ObservatoryConfig config;
  config.runtime.worker_threads = 0;
  config.history.max_queues = 16;
  config.history.max_events_total = 2048;
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

std::string write_document_file(const std::string& label, const std::string& contents) {
  const std::string directory = qobs::test::make_temp_directory(label);
  const std::string path = directory + "/document.ndjson";
  std::FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    return {};
  }
  (void)std::fwrite(contents.data(), 1u, contents.size(), file);
  std::fclose(file);
  return path;
}

}  // namespace

QOBS_TEST(transport, loopback_round_trip_between_a_real_socket_pair) {
  std::unique_ptr<Observatory> runtime = make_runtime();
  QOBS_REQUIRE(runtime != nullptr);
  TransportServer server(*runtime, TransportLimits{});
  QOBS_CHECK_STATUS(server.listen_on("127.0.0.1", 0, 8));
  const std::uint16_t port = server.local_port();
  QOBS_CHECK(port != 0u);

  StopSource stop;
  std::thread serving([&] { (void)server.serve(stop.token()); });

  TransportClient client;
  QOBS_CHECK_STATUS(client.connect_to("127.0.0.1", port, "transport-test"));
  QOBS_CHECK(client.welcome().find("queue-observatory") != std::string::npos);
  QOBS_CHECK(client.welcome().find("observation only") != std::string::npos);

  BatchReply reply;
  QOBS_CHECK_STATUS(client.send_batch(transport_document(1, 3), reply));
  QOBS_CHECK_EQ(reply.report.admission.accepted, 3u);
  QOBS_CHECK(reply.report.accepted);

  std::string status_payload;
  QOBS_CHECK_STATUS(client.request_status(status_payload));
  QOBS_CHECK(status_payload.find("\"queues\":1") != std::string::npos);

  QOBS_CHECK_STATUS(client.close());
  stop.request_stop();
  serving.join();
  const TransportServerStats stats = server.stats();
  QOBS_CHECK_EQ(stats.connections_accepted, 1u);
  QOBS_CHECK_EQ(stats.batches_applied, 1u);
  QOBS_CHECK_EQ(stats.frames_rejected, 0u);
  QOBS_CHECK_EQ(runtime->status().counters.samples_accepted, 3u);
  QOBS_CHECK_STATUS(runtime->stop());
}

QOBS_TEST(transport, an_independent_server_process_accepts_a_batch) {
  // The listening socket is created here and inherited by the child, so the
  // test never waits for a port announcement and never guesses when the server
  // is ready.
  auto listening = Socket::listen_on("127.0.0.1", 0, 8);
  QOBS_REQUIRE(listening.has_value());
  Socket socket = std::move(listening).value();
  const std::uint16_t port = socket.local_port();
  QOBS_REQUIRE(port != 0u);
  QOBS_CHECK_STATUS(socket.make_inheritable());

  ChildProcess server;
  const std::string handle = std::to_string(static_cast<unsigned long long>(socket.native_handle()));
  const bool spawned = server.spawn(QOBS_TRANSPORT_SERVER_EXE,
                                    {"--listen-handle", handle, "--exit-after-batches", "1"}, true);
  QOBS_CHECK(spawned);
  QOBS_REQUIRE(spawned);
  socket.close();

  TransportClient client;
  const Status connected = client.connect_to("127.0.0.1", port, "process-test");
  QOBS_CHECK_STATUS(connected);
  QOBS_REQUIRE(connected.ok());
  BatchReply reply;
  QOBS_CHECK_STATUS(client.send_batch(transport_document(1, 2), reply));
  QOBS_CHECK_EQ(reply.report.admission.accepted, 2u);
  QOBS_CHECK_STATUS(client.close());

  const int exit_code = server.wait();
  QOBS_CHECK_EQ(exit_code, 0);
}

QOBS_TEST(transport, an_independent_client_process_is_served) {
  std::unique_ptr<Observatory> runtime = make_runtime();
  QOBS_REQUIRE(runtime != nullptr);
  TransportServer server(*runtime, TransportLimits{});
  QOBS_CHECK_STATUS(server.listen_on("127.0.0.1", 0, 8));
  const std::uint16_t port = server.local_port();
  QOBS_REQUIRE(port != 0u);

  const std::string path = write_document_file("transport-client", transport_document(1, 4));
  QOBS_REQUIRE(!path.empty());

  StopSource stop;
  std::atomic<bool> serving{true};
  std::thread server_thread([&] {
    (void)server.serve(stop.token());
    serving.store(false);
  });

  ChildProcess client;
  const bool spawned = client.spawn(
      QOBS_TRANSPORT_CLIENT_EXE,
      {"--host", "127.0.0.1", "--port", std::to_string(port), "--document", path,
       "--expect-accepted", "4"},
      false);
  QOBS_CHECK(spawned);
  QOBS_REQUIRE(spawned);
  const int exit_code = client.wait();
  QOBS_CHECK_EQ(exit_code, 0);

  stop.request_stop();
  server_thread.join();
  QOBS_CHECK(!serving.load());
  QOBS_CHECK_EQ(runtime->status().counters.samples_accepted, 4u);
  const TransportServerStats stats = server.stats();
  QOBS_CHECK_EQ(stats.connections_accepted, 1u);
  QOBS_CHECK_EQ(stats.batches_applied, 1u);
  QOBS_CHECK_STATUS(runtime->stop());
}
