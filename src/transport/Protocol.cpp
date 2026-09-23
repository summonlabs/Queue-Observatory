#include "qobs/transport/Protocol.hpp"

#include <cstring>

#include "qobs/core/Checked.hpp"
#include "qobs/core/Crc32c.hpp"
#include "qobs/version.hpp"

namespace qobs {
namespace transport {
namespace {

void store_u16(std::byte* out, std::uint16_t value) {
  out[0] = static_cast<std::byte>(value & 0xFFu);
  out[1] = static_cast<std::byte>((value >> 8u) & 0xFFu);
}

void store_u32(std::byte* out, std::uint32_t value) {
  for (std::size_t index = 0; index < 4u; ++index) {
    out[index] = static_cast<std::byte>((value >> (8u * index)) & 0xFFu);
  }
}

std::uint16_t load_u16(const std::byte* in) {
  return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(in[0]) |
                                    (std::to_integer<std::uint8_t>(in[1]) << 8u));
}

std::uint32_t load_u32(const std::byte* in) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4u; ++index) {
    value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(in[index])) << (8u * index);
  }
  return value;
}

}  // namespace

std::string_view to_string(FrameType type) noexcept {
  switch (type) {
    case FrameType::Hello:
      return "hello";
    case FrameType::Welcome:
      return "welcome";
    case FrameType::Batch:
      return "batch";
    case FrameType::BatchResult:
      return "batch_result";
    case FrameType::StatusRequest:
      return "status_request";
    case FrameType::StatusReply:
      return "status_reply";
    case FrameType::Bye:
      return "bye";
    case FrameType::Error:
      return "error";
  }
  return "unknown";
}

bool is_known_frame_type(std::uint8_t raw) noexcept {
  return raw >= static_cast<std::uint8_t>(FrameType::Hello) &&
         raw <= static_cast<std::uint8_t>(FrameType::Error);
}

Status encode_frame(const Frame& frame, const TransportLimits& limits, std::vector<std::byte>& out) {
  const Status limits_status = validate_limits(limits);
  if (!limits_status.ok()) {
    return limits_status;
  }
  if (frame.payload.size() > limits.max_frame_bytes) {
    return Status::failure(ErrorCode::LimitExceeded,
                           "the frame payload exceeds the configured frame size");
  }
  std::vector<std::byte> header(kFrameHeaderBytes, std::byte{0});
  store_u32(header.data(), kFrameMagic);
  store_u16(header.data() + 4, kFrameVersion);
  header[6] = static_cast<std::byte>(static_cast<std::uint8_t>(frame.type));
  header[7] = static_cast<std::byte>(frame.flags);
  store_u32(header.data() + 8, static_cast<std::uint32_t>(frame.payload.size()));

  Crc32c crc;
  crc.update(std::span<const std::byte>(header.data() + 4, 8u));
  crc.update(frame.payload);
  store_u32(header.data() + 12, crc.value());

  out.clear();
  out.reserve(header.size() + frame.payload.size());
  out.insert(out.end(), header.begin(), header.end());
  const auto* payload_bytes = reinterpret_cast<const std::byte*>(frame.payload.data());
  out.insert(out.end(), payload_bytes, payload_bytes + frame.payload.size());
  return Status::success();
}

Status decode_frame(std::span<const std::byte> buffer, const TransportLimits& limits,
                    DecodeFrameResult& result) {
  result = DecodeFrameResult{};
  const Status limits_status = validate_limits(limits);
  if (!limits_status.ok()) {
    return limits_status;
  }
  if (buffer.size() < kFrameHeaderBytes) {
    result.complete = false;
    return Status::success();
  }
  const std::uint32_t magic = load_u32(buffer.data());
  if (magic != kFrameMagic) {
    result.diagnostic = "the frame magic does not match";
    return Status::failure(ErrorCode::IntegrityFailure, result.diagnostic);
  }
  const std::uint16_t version = load_u16(buffer.data() + 4);
  if (version != kFrameVersion) {
    result.diagnostic = "the frame was written in an unsupported protocol version";
    return Status::failure(ErrorCode::VersionMismatch, result.diagnostic);
  }
  const std::uint8_t raw_type = std::to_integer<std::uint8_t>(buffer[6]);
  if (!is_known_frame_type(raw_type)) {
    result.diagnostic = "the frame declares an unknown type";
    return Status::failure(ErrorCode::NotSupported, result.diagnostic);
  }
  const std::uint32_t length = load_u32(buffer.data() + 8);
  if (length > limits.max_frame_bytes) {
    result.diagnostic = "the frame declares a payload larger than the configured limit";
    return Status::failure(ErrorCode::LimitExceeded, result.diagnostic);
  }
  const auto total = checked::add_u64(kFrameHeaderBytes, length);
  if (!total.has_value()) {
    result.diagnostic = "the frame length overflows";
    return Status::failure(ErrorCode::Overflow, result.diagnostic);
  }
  if (static_cast<std::uint64_t>(buffer.size()) < *total) {
    result.complete = false;
    return Status::success();
  }
  const auto stored_crc = load_u32(buffer.data() + 12);
  Crc32c crc;
  crc.update(std::span<const std::byte>(buffer.data() + 4, 8u));
  crc.update(std::span<const std::byte>(buffer.data() + kFrameHeaderBytes, length));
  if (crc.value() != stored_crc) {
    result.diagnostic = "the frame checksum does not match its contents";
    return Status::failure(ErrorCode::IntegrityFailure, result.diagnostic);
  }
  result.complete = true;
  result.consumed = *total;
  result.frame.type = static_cast<FrameType>(raw_type);
  result.frame.flags = std::to_integer<std::uint8_t>(buffer[7]);
  result.frame.payload.assign(
      reinterpret_cast<const char*>(buffer.data() + kFrameHeaderBytes), length);
  return Status::success();
}

Status FrameReader::append(std::span<const std::byte> bytes) {
  const auto projected = checked::add_u64(static_cast<std::uint64_t>(buffer_.size()),
                                          static_cast<std::uint64_t>(bytes.size()));
  if (!projected.has_value() || *projected > limits_.max_connection_buffer_bytes) {
    return Status::failure(ErrorCode::LimitExceeded,
                           "the connection buffer would exceed its configured bound");
  }
  buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
  return Status::success();
}

Status FrameReader::next(Frame& frame, bool& produced) {
  produced = false;
  if (buffer_.empty()) {
    return Status::success();
  }
  DecodeFrameResult decoded;
  const Status status = decode_frame(buffer_, limits_, decoded);
  if (!status.ok()) {
    return status;
  }
  if (!decoded.complete) {
    return Status::success();
  }
  frame = std::move(decoded.frame);
  produced = true;
  buffer_.erase(buffer_.begin(),
                buffer_.begin() + static_cast<std::ptrdiff_t>(decoded.consumed));
  return Status::success();
}

}  // namespace transport
}  // namespace qobs
