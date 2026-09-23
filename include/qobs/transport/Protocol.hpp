#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "qobs/core/Limits.hpp"
#include "qobs/core/Result.hpp"

namespace qobs {

/// Framing for the Queue Observatory observation transport.
///
/// The transport carries exactly the canonical observation document defined by
/// qobs/ingest/Wire.hpp. It is a real TCP stream between real processes; the
/// runtime makes no claim about any other transport, and in particular no claim
/// about RDMA, InfiniBand or any in-band telemetry channel.
namespace transport {

inline constexpr std::uint32_t kFrameMagic = 0x514F4253u;  // "QOBS"
inline constexpr std::uint16_t kFrameVersion = 1u;
inline constexpr std::size_t kFrameHeaderBytes = 16u;

enum class FrameType : std::uint8_t {
  Hello = 1,
  Welcome = 2,
  Batch = 3,
  BatchResult = 4,
  StatusRequest = 5,
  StatusReply = 6,
  Bye = 7,
  Error = 8,
};

[[nodiscard]] std::string_view to_string(FrameType type) noexcept;
[[nodiscard]] bool is_known_frame_type(std::uint8_t raw) noexcept;

struct Frame {
  FrameType type{FrameType::Hello};
  std::uint8_t flags{0};
  std::string payload{};
};

/// Encode one frame. The payload must fit in the configured frame size.
[[nodiscard]] Status encode_frame(const Frame& frame, const TransportLimits& limits,
                                  std::vector<std::byte>& out);

/// Outcome of attempting to decode one frame from the front of a buffer.
///
/// When the buffer holds only part of a frame, the result reports complete as
/// false and consumes nothing, so a caller can simply wait for more bytes.
struct DecodeFrameResult {
  bool complete{false};
  std::uint64_t consumed{0};
  Frame frame{};
  std::string diagnostic{};
};

[[nodiscard]] Status decode_frame(std::span<const std::byte> buffer, const TransportLimits& limits,
                                  DecodeFrameResult& result);

/// Incremental reassembler. Bytes are appended as they arrive; complete frames
/// are produced in order. The buffer never grows past the configured bound.
class FrameReader {
 public:
  explicit FrameReader(TransportLimits limits) : limits_(limits) {}

  [[nodiscard]] Status append(std::span<const std::byte> bytes);
  /// Extract the next complete frame. Reports produced as false when more bytes
  /// are needed.
  [[nodiscard]] Status next(Frame& frame, bool& produced);
  [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size(); }
  void clear() noexcept { buffer_.clear(); }

 private:
  TransportLimits limits_{};
  std::vector<std::byte> buffer_{};
};

}  // namespace transport
}  // namespace qobs
