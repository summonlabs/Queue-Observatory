#include "qobs/transport/Socket.hpp"

#include <array>
#include <cstring>
#include <mutex>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "qobs/core/Checked.hpp"

namespace qobs {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kNativeInvalid = INVALID_SOCKET;
constexpr int kSendFlags = 0;

std::once_flag g_wsa_once;
int g_wsa_result = 0;

void ensure_wsa() {
  std::call_once(g_wsa_once, [] {
    WSADATA data{};
    g_wsa_result = WSAStartup(MAKEWORD(2, 2), &data);
  });
}

NativeSocket to_native(std::uintptr_t handle) { return static_cast<NativeSocket>(handle); }
std::uintptr_t from_native(NativeSocket socket) { return static_cast<std::uintptr_t>(socket); }
#else
using NativeSocket = int;
constexpr NativeSocket kNativeInvalid = -1;
constexpr int kSendFlags = MSG_NOSIGNAL;

void ensure_wsa() {}

NativeSocket to_native(std::uintptr_t handle) { return static_cast<NativeSocket>(handle); }
std::uintptr_t from_native(NativeSocket socket) { return static_cast<std::uintptr_t>(socket); }
#endif

bool socket_valid(NativeSocket socket) { return socket != kNativeInvalid; }

void close_native(NativeSocket socket) {
  if (!socket_valid(socket)) {
    return;
  }
#ifdef _WIN32
  ::closesocket(socket);
#else
  ::close(socket);
#endif
}

}  // namespace

std::string last_socket_error() {
#ifdef _WIN32
  const int code = WSAGetLastError();
  return "winsock error " + std::to_string(code);
#else
  return std::string(std::strerror(errno));
#endif
}

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_) { other.handle_ = kInvalidHandle; }

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = kInvalidHandle;
  }
  return *this;
}

bool Socket::valid() const noexcept { return handle_ != kInvalidHandle; }

void Socket::close() noexcept {
  if (!valid()) {
    return;
  }
  close_native(to_native(handle_));
  handle_ = kInvalidHandle;
}

Status Socket::initialise() {
  ensure_wsa();
#ifdef _WIN32
  if (g_wsa_result != 0) {
    return Status::failure(ErrorCode::IoError,
                           "the platform networking layer could not be started: " +
                               last_socket_error());
  }
#endif
  return Status::success();
}

Result<Socket> Socket::listen_on(const std::string& host, std::uint16_t port, int backlog) {
  QOBS_TRY_RESULT(initialise());
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* resolved = nullptr;
  const std::string service = std::to_string(port);
  const char* node = host.empty() ? nullptr : host.c_str();
  if (::getaddrinfo(node, service.c_str(), &hints, &resolved) != 0 || resolved == nullptr) {
    return Result<Socket>::failure(ErrorCode::IoError,
                                   "the listen address could not be resolved: " + host);
  }
  NativeSocket native = kNativeInvalid;
  for (addrinfo* candidate = resolved; candidate != nullptr; candidate = candidate->ai_next) {
    native = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (!socket_valid(native)) {
      continue;
    }
    const int reuse = 1;
    ::setsockopt(native, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    if (::bind(native, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0 &&
        ::listen(native, backlog) == 0) {
      break;
    }
    close_native(native);
    native = kNativeInvalid;
  }
  ::freeaddrinfo(resolved);
  if (!socket_valid(native)) {
    return Result<Socket>::failure(ErrorCode::IoError,
                                   "the listening socket could not be created: " +
                                       last_socket_error());
  }
  return Socket(from_native(native));
}

Result<Socket> Socket::connect_to(const std::string& host, std::uint16_t port) {
  QOBS_TRY_RESULT(initialise());
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* resolved = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(host.c_str(), service.c_str(), &hints, &resolved) != 0 || resolved == nullptr) {
    return Result<Socket>::failure(ErrorCode::IoError,
                                   "the remote address could not be resolved: " + host);
  }
  NativeSocket native = kNativeInvalid;
  for (addrinfo* candidate = resolved; candidate != nullptr; candidate = candidate->ai_next) {
    native = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (!socket_valid(native)) {
      continue;
    }
    if (::connect(native, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0) {
      break;
    }
    close_native(native);
    native = kNativeInvalid;
  }
  ::freeaddrinfo(resolved);
  if (!socket_valid(native)) {
    return Result<Socket>::failure(ErrorCode::IoError,
                                   "the connection could not be established: " + host + ":" +
                                       service + " (" + last_socket_error() + ")");
  }
  return Socket(from_native(native));
}

Result<Socket> Socket::accept(std::string& peer) {
  if (!valid()) {
    return Result<Socket>::failure(ErrorCode::NotRunning,
                                   "cannot accept on a socket that is not listening");
  }
  sockaddr_storage address{};
#ifdef _WIN32
  int length = static_cast<int>(sizeof(address));
#else
  socklen_t length = sizeof(address);
#endif
  const NativeSocket accepted =
      ::accept(to_native(handle_), reinterpret_cast<sockaddr*>(&address), &length);
  if (!socket_valid(accepted)) {
    return Result<Socket>::failure(ErrorCode::IoError,
                                   "the connection could not be accepted: " + last_socket_error());
  }
  std::array<char, 64> text{};
  if (address.ss_family == AF_INET) {
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(&address);
    ::inet_ntop(AF_INET, &ipv4->sin_addr, text.data(), static_cast<unsigned>(text.size()));
    peer = std::string(text.data()) + ":" + std::to_string(ntohs(ipv4->sin_port));
  } else {
    peer = "unknown";
  }
  return Socket(from_native(accepted));
}

std::uint16_t Socket::local_port() const {
  if (!valid()) {
    return 0;
  }
  sockaddr_in address{};
#ifdef _WIN32
  int length = static_cast<int>(sizeof(address));
#else
  socklen_t length = sizeof(address);
#endif
  if (::getsockname(to_native(handle_), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    return 0;
  }
  return ntohs(address.sin_port);
}

Status Socket::send_all(std::span<const std::byte> bytes) {
  if (!valid()) {
    return Status::failure(ErrorCode::NotRunning, "cannot send on a closed socket");
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto remaining = bytes.size() - offset;
    const int chunk = static_cast<int>(remaining > 1u << 20 ? 1u << 20 : remaining);
    const int sent = ::send(to_native(handle_),
                            reinterpret_cast<const char*>(bytes.data() + offset), chunk, kSendFlags);
    if (sent <= 0) {
      return Status::failure(ErrorCode::IoError,
                             "the frame could not be sent: " + last_socket_error());
    }
    offset += static_cast<std::size_t>(sent);
  }
  return Status::success();
}

Status Socket::receive(std::span<std::byte> buffer, std::size_t& count) {
  count = 0;
  if (!valid()) {
    return Status::failure(ErrorCode::NotRunning, "cannot receive on a closed socket");
  }
  if (buffer.empty()) {
    return Status::success();
  }
  const int chunk = static_cast<int>(buffer.size() > 1u << 20 ? 1u << 20 : buffer.size());
  const int received =
      ::recv(to_native(handle_), reinterpret_cast<char*>(buffer.data()), chunk, 0);
  if (received == 0) {
    return Status::success();
  }
  if (received < 0) {
    return Status::failure(ErrorCode::IoError,
                           "the connection failed while receiving: " + last_socket_error());
  }
  count = static_cast<std::size_t>(received);
  return Status::success();
}

Status Socket::make_inheritable() {
  if (!valid()) {
    return Status::failure(ErrorCode::NotRunning, "cannot mark a closed socket as inheritable");
  }
#ifdef _WIN32
  if (::SetHandleInformation(reinterpret_cast<HANDLE>(to_native(handle_)), HANDLE_FLAG_INHERIT,
                             HANDLE_FLAG_INHERIT) == 0) {
    return Status::failure(ErrorCode::IoError, "the socket could not be marked inheritable");
  }
#else
  const int flags = ::fcntl(to_native(handle_), F_GETFD);
  if (flags < 0 || ::fcntl(to_native(handle_), F_SETFD, flags & ~FD_CLOEXEC) < 0) {
    return Status::failure(ErrorCode::IoError, "the socket could not be marked inheritable");
  }
#endif
  return Status::success();
}

}  // namespace qobs
