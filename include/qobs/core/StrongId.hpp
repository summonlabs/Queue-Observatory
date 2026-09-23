#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include "qobs/core/Result.hpp"

namespace qobs {

/// Maximum accepted length for any name-valued identity.
inline constexpr std::size_t kMaxIdentityLength = 128u;

/// A strongly typed integral identity.
///
/// Distinct tags make device/port/queue/traffic-class/scheduling-class/source/
/// generation/incarnation/sequence/revision values mutually incompatible at
/// compile time, so a queue index can never be silently used where a port
/// index is meant.
template <class Tag, class Value = std::uint64_t>
class StrongId {
 public:
  using value_type = Value;
  using tag_type = Tag;

  constexpr StrongId() noexcept = default;
  constexpr explicit StrongId(Value value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr StrongId from_raw(Value value) noexcept { return StrongId(value); }

  [[nodiscard]] constexpr Value value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool valid() const noexcept { return value_ != Value{0}; }

  friend constexpr bool operator==(StrongId lhs, StrongId rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }
  friend constexpr bool operator!=(StrongId lhs, StrongId rhs) noexcept { return !(lhs == rhs); }
  friend constexpr bool operator<(StrongId lhs, StrongId rhs) noexcept {
    return lhs.value_ < rhs.value_;
  }
  friend constexpr bool operator>(StrongId lhs, StrongId rhs) noexcept { return rhs < lhs; }
  friend constexpr bool operator<=(StrongId lhs, StrongId rhs) noexcept { return !(rhs < lhs); }
  friend constexpr bool operator>=(StrongId lhs, StrongId rhs) noexcept { return !(lhs < rhs); }

  [[nodiscard]] std::string to_string() const;
  [[nodiscard]] static std::optional<StrongId> parse(std::string_view text) noexcept;

 private:
  Value value_{0};
};

/// A strongly typed opaque name identity (device names, port names, source
/// names, scheduling class names, incarnation tokens).
///
/// Construction from untrusted text must go through create(), which enforces
/// length and character-set limits.
template <class Tag>
class StrongNameId {
 public:
  using tag_type = Tag;

  StrongNameId() = default;
  explicit StrongNameId(std::string value) : value_(std::move(value)) {}

  /// Validate and construct. Rejects empty text, text longer than
  /// kMaxIdentityLength, and text containing control characters.
  [[nodiscard]] static Result<StrongNameId> create(std::string_view text);

  [[nodiscard]] const std::string& value() const noexcept { return value_; }
  [[nodiscard]] std::string_view view() const noexcept { return value_; }
  [[nodiscard]] bool valid() const noexcept { return !value_.empty(); }

  friend bool operator==(const StrongNameId& lhs, const StrongNameId& rhs) noexcept {
    return lhs.value_ == rhs.value_;
  }
  friend bool operator!=(const StrongNameId& lhs, const StrongNameId& rhs) noexcept {
    return !(lhs == rhs);
  }
  friend bool operator<(const StrongNameId& lhs, const StrongNameId& rhs) noexcept {
    return lhs.value_ < rhs.value_;
  }

  [[nodiscard]] std::string to_string() const { return value_; }

 private:
  std::string value_{};
};

// ---------------------------------------------------------------------------
// Identity tags. Each tag is a distinct, incomplete type.
// ---------------------------------------------------------------------------
struct DeviceTag;
struct PortTag;
struct QueueTag;
struct TrafficClassTag;
struct SchedulingClassTag;
struct SourceTag;
struct GenerationTag;
struct IncarnationTag;
struct SequenceTag;
struct RevisionTag;
struct ClockDomainTag;
struct BatchTag;
struct RecordTag;
struct QueueNameTag;

using DeviceId = StrongNameId<DeviceTag>;
using PortId = StrongNameId<PortTag>;
using SchedulingClassId = StrongNameId<SchedulingClassTag>;
using SourceId = StrongNameId<SourceTag>;
using IncarnationId = StrongNameId<IncarnationTag>;
using ClockDomainId = StrongNameId<ClockDomainTag>;
using QueueNameId = StrongNameId<QueueNameTag>;

using QueueId = StrongId<QueueTag, std::uint32_t>;
using TrafficClassId = StrongId<TrafficClassTag, std::uint16_t>;
using GenerationId = StrongId<GenerationTag, std::uint64_t>;
using SourceSequence = StrongId<SequenceTag, std::uint64_t>;
using Revision = StrongId<RevisionTag, std::uint64_t>;
using BatchId = StrongId<BatchTag, std::uint64_t>;
using RecordId = StrongId<RecordTag, std::uint64_t>;

/// Ordinal assigned by the runtime the first time it observes an opaque
/// incarnation token. Ordinals give a total order to otherwise unordered
/// tokens, which is what makes incarnation replay fencing possible.
using IncarnationOrdinal = std::uint64_t;

}  // namespace qobs

namespace std {

template <class Tag, class Value>
struct hash<qobs::StrongId<Tag, Value>> {
  std::size_t operator()(const qobs::StrongId<Tag, Value>& id) const noexcept {
    return std::hash<Value>{}(id.value());
  }
};

template <class Tag>
struct hash<qobs::StrongNameId<Tag>> {
  std::size_t operator()(const qobs::StrongNameId<Tag>& id) const noexcept {
    return std::hash<std::string>{}(id.value());
  }
};

}  // namespace std
