#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "qobs/core/Result.hpp"

namespace qobs {

/// A thin, portable TCP socket wrapper.
///
/// Queue Observatory only ever uses loopback or explicitly configured TCP
/// endpoints. Nothing here reaches for a privileged or vendor-specific
/// interface.
class Socket {
 public:
  Socket() = default;
  explicit Socket(std::uintptr_t handle) : handle_(handle) {}
  ~Socket();

  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;

  [[nodiscard]] bool valid() const noexcept;

  /// Close the socket. Safe to call more than once.
  void close() noexcept;

  /// Start the platform networking layer. Idempotent and thread safe.
  [[nodiscard]] static Status initialise();

  [[nodiscard]] static Result<Socket> listen_on(const std::string& host, std::uint16_t port,
                                                int backlog);
  [[nodiscard]] static Result<Socket> connect_to(const std::string& host, std::uint16_t port);

  [[nodiscard]] Result<Socket> accept(std::string& peer);
  [[nodiscard]] std::uint16_t local_port() const;

  [[nodiscard]] Status send_all(std::span<const std::byte> bytes);
  /// Receive at least one byte. Returns Ok with count zero only at end of
  /// stream.
  [[nodiscard]] Status receive(std::span<std::byte> buffer, std::size_t& count);

  /// Mark the socket as inheritable so that a child process can accept on it.
  [[nodiscard]] Status make_inheritable();
  [[nodiscard]] std::uintptr_t native_handle() const noexcept { return handle_; }

  /// Adopt an already-open handle, for example one inherited from a parent
  /// process. The socket takes ownership and closes it on destruction.
  [[nodiscard]] static Socket adopt(std::uintptr_t handle) noexcept { return Socket(handle); }

 private:
  static constexpr std::uintptr_t kInvalidHandle = static_cast<std::uintptr_t>(~std::uintptr_t{0});
  std::uintptr_t handle_{kInvalidHandle};
};

/// Human-readable description of the last socket error.
[[nodiscard]] std::string last_socket_error();

}  // namespace qobs
