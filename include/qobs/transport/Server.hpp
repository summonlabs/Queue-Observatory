#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "qobs/core/Cancellation.hpp"
#include "qobs/core/Limits.hpp"
#include "qobs/core/Result.hpp"
#include "qobs/runtime/Observatory.hpp"
#include "qobs/transport/Protocol.hpp"
#include "qobs/transport/Socket.hpp"

namespace qobs {

using transport::Frame;
using transport::FrameReader;
using transport::FrameType;

struct TransportServerStats {
  std::uint64_t connections_accepted{0};
  std::uint64_t connections_rejected{0};
  std::uint64_t frames_received{0};
  std::uint64_t frames_rejected{0};
  std::uint64_t batches_applied{0};
  std::uint64_t batch_failures{0};
};

/// The observation ingest server.
///
/// It accepts the canonical document over TCP, hands it to the runtime, and
/// answers with what the runtime actually did. It exposes no interface that can
/// change device configuration: the only thing it accepts is evidence.
class TransportServer {
 public:
  TransportServer(Observatory& runtime, TransportLimits limits);
  ~TransportServer();

  TransportServer(const TransportServer&) = delete;
  TransportServer& operator=(const TransportServer&) = delete;

  [[nodiscard]] Status listen_on(const std::string& host, std::uint16_t port, int backlog);
  /// Adopt a listening socket created elsewhere, for example one inherited from
  /// a parent process. Ownership transfers to the server.
  [[nodiscard]] Status adopt(Socket listening);
  [[nodiscard]] std::uint16_t local_port() const;

  /// Accept and serve connections until the token is signalled. Returns once
  /// every connection thread has been joined.
  [[nodiscard]] Status serve(const StopToken& token);
  /// Ask the accept loop to stop. Safe to call from another thread.
  void request_stop() noexcept;
  /// Stop serving once this many batch frames have been applied. Zero means
  /// never. Used by the process level transport tests so that a server process
  /// terminates deterministically without a timeout.
  void set_stop_after_batches(std::size_t count) noexcept;

  [[nodiscard]] TransportServerStats stats() const;

 private:
  [[nodiscard]] Status handle_connection(Socket connection, const std::string& peer);
  [[nodiscard]] Status handle_frame(Socket& connection, const Frame& frame, bool& keep_going);

  Observatory* runtime_{};
  TransportLimits limits_{};
  Socket listening_{};
  std::atomic<bool> stop_requested_{false};
  std::atomic<std::uint64_t> next_batch_ticket_{1};
  std::size_t stop_after_batches_{0};
  mutable std::mutex mutex_{};
  std::vector<std::thread> connection_threads_{};
  TransportServerStats stats_{};
};

/// Reply produced for one batch frame.
struct BatchReply {
  std::uint64_t ticket{0};
  IngestReport report{};
  std::string error_code{};
  std::string error_message{};
};

/// The observation ingest client.
class TransportClient {
 public:
  TransportClient() = default;
  ~TransportClient();

  TransportClient(const TransportClient&) = delete;
  TransportClient& operator=(const TransportClient&) = delete;
  TransportClient(TransportClient&&) noexcept = default;
  TransportClient& operator=(TransportClient&&) noexcept = default;

  [[nodiscard]] Status connect_to(const std::string& host, std::uint16_t port,
                                  std::string client_name);
  [[nodiscard]] Status send_batch(std::string_view document, BatchReply& reply);
  [[nodiscard]] Status request_status(std::string& payload);
  [[nodiscard]] Status close();

  [[nodiscard]] const std::string& welcome() const noexcept { return welcome_; }
  [[nodiscard]] bool connected() const noexcept { return connection_.valid(); }

 private:
  [[nodiscard]] Status send_frame(FrameType type, std::string payload);
  [[nodiscard]] Status receive_frame(Frame& frame);

  TransportLimits limits_{};
  Socket connection_{};
  transport::FrameReader reader_{TransportLimits{}};
  std::string welcome_{};
  std::uint64_t next_ticket_{1};
};

}  // namespace qobs
