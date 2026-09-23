#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "qobs/core/Result.hpp"
#include "qobs/core/StrongId.hpp"

namespace qobs {

/// The identity of one queue: a queue index on a port of a device.
///
/// Traffic class and scheduling class are deliberately *not* part of the
/// identity. They are attributes that may be observed in a sample, resolved
/// from class metadata, or simply unknown, and keeping them out of the key is
/// what lets the runtime report which of those happened.
struct QueuePath {
  DeviceId device{};
  PortId port{};
  QueueId queue{};

  /// A queue path is valid when it names a device and a port.
  ///
  /// The queue index is deliberately not part of this test: index zero is an
  /// ordinary queue on every device that numbers its queues from zero, so a
  /// zero index must never be mistaken for an unset one.
  [[nodiscard]] bool valid() const noexcept { return device.valid() && port.valid(); }

  friend bool operator==(const QueuePath& lhs, const QueuePath& rhs) noexcept {
    return lhs.device == rhs.device && lhs.port == rhs.port && lhs.queue == rhs.queue;
  }
  friend bool operator!=(const QueuePath& lhs, const QueuePath& rhs) noexcept {
    return !(lhs == rhs);
  }
  friend bool operator<(const QueuePath& lhs, const QueuePath& rhs) noexcept {
    if (lhs.device != rhs.device) {
      return lhs.device < rhs.device;
    }
    if (lhs.port != rhs.port) {
      return lhs.port < rhs.port;
    }
    return lhs.queue < rhs.queue;
  }
  friend bool operator>(const QueuePath& lhs, const QueuePath& rhs) noexcept { return rhs < lhs; }

  /// device/port/queue
  [[nodiscard]] std::string to_string() const;
  [[nodiscard]] static Result<QueuePath> parse(std::string_view text);
};

/// The identity of one port, used for sibling correlation.
struct PortPath {
  DeviceId device{};
  PortId port{};

  [[nodiscard]] bool valid() const noexcept { return device.valid() && port.valid(); }

  friend bool operator==(const PortPath& lhs, const PortPath& rhs) noexcept {
    return lhs.device == rhs.device && lhs.port == rhs.port;
  }
  friend bool operator!=(const PortPath& lhs, const PortPath& rhs) noexcept {
    return !(lhs == rhs);
  }
  friend bool operator<(const PortPath& lhs, const PortPath& rhs) noexcept {
    if (lhs.device != rhs.device) {
      return lhs.device < rhs.device;
    }
    return lhs.port < rhs.port;
  }

  [[nodiscard]] std::string to_string() const;
};

}  // namespace qobs

namespace std {
template <>
struct hash<qobs::QueuePath> {
  std::size_t operator()(const qobs::QueuePath& path) const noexcept {
    std::size_t seed = std::hash<std::string>{}(path.device.value());
    seed ^= std::hash<std::string>{}(path.port.value()) + 0x9e3779b97f4a7c15ull + (seed << 6u) +
            (seed >> 2u);
    seed ^= std::hash<std::uint32_t>{}(path.queue.value()) + 0x9e3779b97f4a7c15ull + (seed << 6u) +
            (seed >> 2u);
    return seed;
  }
};
}  // namespace std
