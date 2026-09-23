#include "qobs/model/Identity.hpp"

#include "qobs/core/Text.hpp"

namespace qobs {
namespace {

/// Canonical queue path separator.
///
/// A slash would be the obvious choice, but real interface names contain one
/// ("Ethernet1/1"), so the canonical text form uses a character that device and
/// port identities are forbidden to contain. The structured form used on the
/// wire and in the API carries the three components separately and never has to
/// worry about this at all.
constexpr char kSeparator = '|';

}  // namespace

std::string QueuePath::to_string() const {
  std::string out;
  out.reserve(device.value().size() + port.value().size() + 24u);
  out.append(device.value());
  out.push_back(kSeparator);
  out.append(port.value());
  out.push_back(kSeparator);
  out.append(queue.to_string());
  return out;
}

Result<QueuePath> QueuePath::parse(std::string_view text) {
  const std::vector<std::string_view> parts = text::split(text, kSeparator);
  if (parts.size() != 3u) {
    return Result<QueuePath>::failure(
        ErrorCode::ParseError,
        "a queue path must have the form device/port/queue with exactly two separators");
  }
  QOBS_TRY_ASSIGN(device, DeviceId::create(parts[0]));
  QOBS_TRY_ASSIGN(port, PortId::create(parts[1]));
  const auto queue = QueueId::parse(parts[2]);
  if (!queue.has_value()) {
    return Result<QueuePath>::failure(ErrorCode::ParseError,
                                      "the queue component of a path must be an unsigned integer");
  }
  QueuePath path;
  path.device = device;
  path.port = port;
  path.queue = *queue;
  return path;
}

std::string PortPath::to_string() const {
  std::string out;
  out.reserve(device.value().size() + port.value().size() + 1u);
  out.append(device.value());
  out.push_back(kSeparator);
  out.append(port.value());
  return out;
}

}  // namespace qobs
