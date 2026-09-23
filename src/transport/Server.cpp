#include "qobs/transport/Server.hpp"

#include <condition_variable>
#include <deque>

#include "qobs/core/Json.hpp"
#include "qobs/core/LockAudit.hpp"
#include "qobs/core/Text.hpp"
#include "qobs/runtime/Report.hpp"
#include "qobs/version.hpp"

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/select.h>
#endif

namespace qobs {
namespace {

constexpr unsigned kAcceptPollMicros = 50000u;

std::string encode_status_payload(const RuntimeStatus& status) {
  JsonWriter writer;
  const Status build = [&]() -> Status {
    QOBS_TRY(writer.begin_object());
    QOBS_TRY(writer.member_string("state", to_string(status.state)));
    QOBS_TRY(writer.member_string("runtime_id", status.runtime_id));
    QOBS_TRY(writer.member_u64("queues", status.queues));
    QOBS_TRY(writer.member_u64("history_bytes", status.history_bytes));
    QOBS_TRY(writer.member_u64("samples_presented", status.counters.samples_presented));
    QOBS_TRY(writer.member_u64("samples_accepted", status.counters.samples_accepted));
    QOBS_TRY(writer.member_u64("samples_fenced", status.counters.samples_fenced));
    QOBS_TRY(writer.member_u64("samples_rejected", status.counters.samples_rejected));
    QOBS_TRY(writer.member_u64("duplicates_suppressed", status.counters.duplicates_suppressed));
    QOBS_TRY(writer.member_u64("counter_wraps", status.counters.counter_wraps));
    QOBS_TRY(writer.member_u64("counter_resets", status.counters.counter_resets));
    QOBS_TRY(writer.member_u64("counter_ambiguous", status.counters.counter_ambiguous));
    QOBS_TRY(writer.member_u64("wire_version", QOBS_WIRE_FORMAT_VERSION));
    QOBS_TRY(writer.member_string("runtime_version", QOBS_VERSION_STRING));
    QOBS_TRY(writer.end_object());
    return Status::success();
  }();
  if (!build.ok()) {
    return "{}";
  }
  return writer.take();
}

std::string encode_batch_result(const BatchReply& reply) {
  JsonWriter writer;
  const Status build = [&]() -> Status {
    QOBS_TRY(writer.begin_object());
    QOBS_TRY(writer.member_u64("ticket", reply.ticket));
    if (!reply.error_code.empty()) {
      QOBS_TRY(writer.member_string("error_code", reply.error_code));
      QOBS_TRY(writer.member_string("error_message", reply.error_message));
    }
    QOBS_TRY(writer.member_u64("lines", reply.report.decode.lines));
    QOBS_TRY(writer.member_u64("samples_decoded", reply.report.decode.sample_count));
    QOBS_TRY(writer.member_u64("metadata_records", reply.report.decode.metadata_records));
    QOBS_TRY(writer.member_u64("malformed", reply.report.decode.malformed));
    QOBS_TRY(writer.member_u64("unsupported", reply.report.decode.unsupported));
    QOBS_TRY(writer.member_u64("version_mismatches", reply.report.decode.version_mismatches));
    QOBS_TRY(writer.member_u64("presented", reply.report.admission.presented));
    QOBS_TRY(writer.member_u64("accepted", reply.report.admission.accepted));
    QOBS_TRY(writer.member_u64("rejected", reply.report.admission.rejected));
    QOBS_TRY(writer.member_u64("fenced", reply.report.admission.fenced));
    QOBS_TRY(writer.member_u64("duplicates", reply.report.admission.duplicates));
    QOBS_TRY(writer.member_u64("queues_created", reply.report.admission.queues_created));
    QOBS_TRY(writer.member_bool("applied", reply.report.accepted));
    QOBS_TRY(writer.end_object());
    return Status::success();
  }();
  if (!build.ok()) {
    return "{}";
  }
  return writer.take();
}

}  // namespace

TransportServer::TransportServer(Observatory& runtime, TransportLimits limits)
    : runtime_(&runtime), limits_(limits) {}

TransportServer::~TransportServer() {
  request_stop();
  listening_.close();
  for (std::thread& worker : connection_threads_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

Status TransportServer::listen_on(const std::string& host, std::uint16_t port, int backlog) {
  QOBS_TRY(validate_limits(limits_));
  auto created = Socket::listen_on(host, port, backlog);
  if (!created.has_value()) {
    return Status(created.error());
  }
  listening_ = std::move(created).value();
  return Status::success();
}

Status TransportServer::adopt(Socket listening) {
  QOBS_TRY(validate_limits(limits_));
  QOBS_TRY(Socket::initialise());
  if (!listening.valid()) {
    return Status::failure(ErrorCode::InvalidArgument, "the adopted socket is not open");
  }
  listening_ = std::move(listening);
  return Status::success();
}

std::uint16_t TransportServer::local_port() const { return listening_.local_port(); }

void TransportServer::request_stop() noexcept { stop_requested_.store(true); }

void TransportServer::set_stop_after_batches(std::size_t count) noexcept {
  stop_after_batches_ = count;
}

TransportServerStats TransportServer::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

Status TransportServer::serve(const StopToken& token) {
  QOBS_TRY(Socket::initialise());
  if (!listening_.valid()) {
    return Status::failure(ErrorCode::NotRunning,
                           "the transport server is not listening on any socket");
  }
  struct PendingConnection {
    Socket socket{};
    std::string peer{};
  };
  std::mutex queue_mutex;
  std::condition_variable queue_cv;
  std::deque<PendingConnection> queue;
  bool closing = false;

  const auto worker = [&]() {
    for (;;) {
      PendingConnection item;
      {
        std::unique_lock<std::mutex> lock(queue_mutex);
        queue_cv.wait(lock, [&] { return closing || !queue.empty(); });
        if (queue.empty()) {
          return;
        }
        item = std::move(queue.front());
        queue.pop_front();
      }
      const Status status = handle_connection(std::move(item.socket), item.peer);
      (void)status;
    }
  };

  const std::size_t pool_size = limits_.max_connections == 0u ? 1u : limits_.max_connections;
  std::vector<std::thread> pool;
  pool.reserve(pool_size);
  for (std::size_t index = 0; index < pool_size; ++index) {
    pool.emplace_back(worker);
  }

  Status result = Status::success();
  while (!token.stop_requested() && !stop_requested_.load()) {
#ifdef _WIN32
    SOCKET native = static_cast<SOCKET>(listening_.native_handle());
#else
    int native = static_cast<int>(listening_.native_handle());
#endif
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(native, &readable);
    timeval wait{};
    wait.tv_sec = 0;
    wait.tv_usec = static_cast<long>(kAcceptPollMicros);
    const int ready = ::select(static_cast<int>(native) + 1, &readable, nullptr, nullptr, &wait);
    if (ready < 0) {
      result = Status::failure(ErrorCode::IoError,
                               "the listening socket became unreadable: " + last_socket_error());
      break;
    }
    if (ready == 0) {
      // No pending connection. Re-check cancellation and the batch budget; the
      // poll interval below only bounds shutdown latency, it never decides
      // correctness.
      if (stop_after_batches_ > 0u) {
        std::lock_guard<std::mutex> stats_lock(mutex_);
        if (stats_.batches_applied >= stop_after_batches_) {
          break;
        }
      }
      continue;
    }
    std::string peer;
    auto accepted = listening_.accept(peer);
    if (!accepted.has_value()) {
      if (token.stop_requested() || stop_requested_.load()) {
        break;
      }
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(queue_mutex);
      if (queue.size() >= pool_size) {
        std::lock_guard<std::mutex> stats_lock(mutex_);
        ++stats_.connections_rejected;
        Frame frame;
        frame.type = FrameType::Error;
        frame.payload = "{\"code\":\"busy\",\"message\":\"the server is at its connection limit\"}";
        std::vector<std::byte> encoded;
        Socket rejected = std::move(accepted).value();
        if (encode_frame(frame, limits_, encoded).ok()) {
          const Status ignored = rejected.send_all(encoded);
          (void)ignored;
        }
        rejected.close();
        continue;
      }
      PendingConnection pending;
      pending.socket = std::move(accepted).value();
      pending.peer = peer;
      queue.push_back(std::move(pending));
    }
    queue_cv.notify_one();
  }

  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    closing = true;
  }
  queue_cv.notify_all();
  for (std::thread& thread : pool) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  listening_.close();
  return result;
}

Status TransportServer::handle_connection(Socket connection, const std::string& peer) {
  (void)peer;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.connections_accepted;
  }
  transport::FrameReader reader(limits_);
  std::vector<std::byte> buffer(64u * 1024u);
  bool keep_going = true;
  while (keep_going) {
    std::size_t received = 0;
    const Status receive_status = connection.receive(buffer, received);
    if (!receive_status.ok()) {
      return receive_status;
    }
    if (received == 0) {
      return Status::success();
    }
    const Status append_status =
        reader.append(std::span<const std::byte>(buffer.data(), received));
    if (!append_status.ok()) {
      return append_status;
    }
    for (;;) {
      Frame frame;
      bool produced = false;
      const Status next_status = reader.next(frame, produced);
      if (!next_status.ok()) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.frames_rejected;
        Frame error;
        error.type = FrameType::Error;
        error.payload = "{\"code\":\"integrity_failure\",\"message\":\"" +
                        text::escape_json(next_status.message()) + "\"}";
        std::vector<std::byte> encoded;
        if (encode_frame(error, limits_, encoded).ok()) {
          const Status ignored = connection.send_all(encoded);
          (void)ignored;
        }
        return Status::success();
      }
      if (!produced) {
        break;
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.frames_received;
      }
      const Status frame_status = handle_frame(connection, frame, keep_going);
      if (!frame_status.ok()) {
        return frame_status;
      }
    }
  }
  return Status::success();
}

Status TransportServer::handle_frame(Socket& connection, const Frame& frame, bool& keep_going) {
  switch (frame.type) {
    case FrameType::Hello: {
      Frame welcome;
      welcome.type = FrameType::Welcome;
      JsonWriter writer;
      QOBS_TRY(writer.begin_object());
      QOBS_TRY(writer.member_string("server", "queue-observatory"));
      QOBS_TRY(writer.member_string("runtime_version", QOBS_VERSION_STRING));
      QOBS_TRY(writer.member_u64("wire_version", QOBS_WIRE_FORMAT_VERSION));
      QOBS_TRY(writer.member_string("runtime_id", runtime_->store().session().runtime_id));
      QOBS_TRY(writer.member_string("boundary",
                                    "observation only: this endpoint accepts evidence and never "
                                    "changes device configuration"));
      QOBS_TRY(writer.end_object());
      welcome.payload = writer.take();
      std::vector<std::byte> encoded;
      QOBS_TRY(encode_frame(welcome, limits_, encoded));
      return connection.send_all(encoded);
    }
    case FrameType::Batch: {
      BatchReply reply;
      reply.ticket = next_batch_ticket_.fetch_add(1);
      const Status status = runtime_->ingest_document(frame.payload, reply.report);
      if (!status.ok()) {
        reply.error_code = std::string(to_string(status.code()));
        reply.error_message = status.message();
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.batch_failures;
      } else {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.batches_applied;
      }
      Frame result;
      result.type = FrameType::BatchResult;
      result.payload = encode_batch_result(reply);
      std::vector<std::byte> encoded;
      QOBS_TRY(encode_frame(result, limits_, encoded));
      return connection.send_all(encoded);
    }
    case FrameType::StatusRequest: {
      Frame reply;
      reply.type = FrameType::StatusReply;
      reply.payload = encode_status_payload(runtime_->status());
      std::vector<std::byte> encoded;
      QOBS_TRY(encode_frame(reply, limits_, encoded));
      return connection.send_all(encoded);
    }
    case FrameType::Bye:
      keep_going = false;
      return Status::success();
    case FrameType::Welcome:
    case FrameType::BatchResult:
    case FrameType::StatusReply:
    case FrameType::Error: {
      Frame error;
      error.type = FrameType::Error;
      error.payload = "{\"code\":\"unexpected_frame\",\"message\":\"the server does not accept " +
                      std::string(transport::to_string(frame.type)) + " frames\"}";
      std::vector<std::byte> encoded;
      QOBS_TRY(encode_frame(error, limits_, encoded));
      return connection.send_all(encoded);
    }
  }
  Frame error;
  error.type = FrameType::Error;
  error.payload = "{\"code\":\"unknown_frame\",\"message\":\"unrecognised frame type\"}";
  std::vector<std::byte> encoded;
  QOBS_TRY(encode_frame(error, limits_, encoded));
  return connection.send_all(encoded);
}

}  // namespace qobs
