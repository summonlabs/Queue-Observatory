#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "qobs/core/Result.hpp"
#include "qobs/core/StrongId.hpp"
#include "qobs/core/Time.hpp"
#include "qobs/model/Counter.hpp"
#include "qobs/model/Evidence.hpp"
#include "qobs/model/Identity.hpp"

namespace qobs {

/// Every quantitative field a queue observation may carry.
///
/// The enumeration is fixed so that a sample can record which fields were
/// actually reported. A field that was not reported is absent, which is a
/// different thing from a field reported as zero.
enum class SampleField : std::uint8_t {
  OccupancyBytes = 0,
  OccupancyCells,
  QueueDepthPackets,
  SharedBufferCells,
  HeadroomCells,
  ReservedCells,
  StaticThresholdCells,
  DynamicThresholdCells,
  MaxDepthPackets,
  HighWatermarkCells,
  EnqueuePackets,
  DequeuePackets,
  DropPackets,
  DropBytes,
  MarkPackets,
  PauseFrames,
  PauseDurationNanos,
  Count,
};

inline constexpr std::size_t kSampleFieldCount = static_cast<std::size_t>(SampleField::Count);

using FieldMask = std::uint64_t;

[[nodiscard]] constexpr FieldMask field_bit(SampleField field) noexcept {
  return FieldMask{1} << static_cast<unsigned>(field);
}
[[nodiscard]] constexpr bool has_field(FieldMask mask, SampleField field) noexcept {
  return (mask & field_bit(field)) != 0u;
}

[[nodiscard]] std::string_view to_string(SampleField field) noexcept;
[[nodiscard]] std::optional<SampleField> parse_sample_field(std::string_view text) noexcept;

/// Counters accumulate and can wrap; gauges are instantaneous.
[[nodiscard]] constexpr bool is_counter_field(SampleField field) noexcept {
  switch (field) {
    case SampleField::EnqueuePackets:
    case SampleField::DequeuePackets:
    case SampleField::DropPackets:
    case SampleField::DropBytes:
    case SampleField::MarkPackets:
    case SampleField::PauseFrames:
    case SampleField::PauseDurationNanos:
      return true;
    case SampleField::OccupancyBytes:
    case SampleField::OccupancyCells:
    case SampleField::QueueDepthPackets:
    case SampleField::SharedBufferCells:
    case SampleField::HeadroomCells:
    case SampleField::ReservedCells:
    case SampleField::StaticThresholdCells:
    case SampleField::DynamicThresholdCells:
    case SampleField::MaxDepthPackets:
    case SampleField::HighWatermarkCells:
    case SampleField::Count:
      return false;
  }
  return false;
}

/// Where the traffic-class and scheduling-class attributes came from.
enum class AttributeOrigin : std::uint8_t {
  Unspecified = 0,
  ObservedInSample,
  ResolvedFromMetadata,
};

[[nodiscard]] std::string_view to_string(AttributeOrigin value) noexcept;

struct QueueClassAttributes {
  std::optional<TrafficClassId> traffic_class{};
  std::optional<SchedulingClassId> scheduling_class{};
  AttributeOrigin origin{AttributeOrigin::Unspecified};
  Revision metadata_revision{};

  friend bool operator==(const QueueClassAttributes&, const QueueClassAttributes&) = default;
};

/// One observation of one queue.
///
/// A sample is a value object: it records exactly what a source claimed, when
/// the source claimed it, when this runtime received it, under which source
/// incarnation and generation, and which fields were present.
struct QueueSample {
  QueuePath queue{};
  QueueClassAttributes classes{};

  SourceId source{};
  IncarnationId incarnation{};
  SourceSequence sequence{};
  SourceAuthority authority{SourceAuthority::Unknown};
  GenerationId generation{};
  Revision revision{};

  ObservationTime observed{};
  ReceiveTime received{};

  FieldMask reported{0};
  /// What the sender declared it would report. Divergence between this and
  /// reported is what makes a sample explicitly incomplete rather than quietly
  /// under-populated.
  FieldMask declared{0};

  /// Wire version declared by the sender.
  std::uint32_t declared_version{0};
  /// Optional sender-side integrity value over the encoded record.
  std::optional<std::uint32_t> declared_crc{};

  [[nodiscard]] bool has(SampleField field) const noexcept { return has_field(reported, field); }

  [[nodiscard]] std::optional<std::uint64_t> value(SampleField field) const noexcept {
    const auto index = static_cast<std::size_t>(field);
    if (index >= kSampleFieldCount) {
      return std::nullopt;
    }
    if (!has_field(reported, field)) {
      return std::nullopt;
    }
    return values_[index];
  }

  void set_value(SampleField field, std::uint64_t value) noexcept {
    const auto index = static_cast<std::size_t>(field);
    if (index >= kSampleFieldCount) {
      return;
    }
    values_[index] = value;
    reported = reported | field_bit(field);
  }

  void clear_value(SampleField field) noexcept {
    const auto index = static_cast<std::size_t>(field);
    if (index >= kSampleFieldCount) {
      return;
    }
    values_[index] = 0;
    reported = reported & ~field_bit(field);
  }

  [[nodiscard]] std::uint64_t raw_value_or_zero(SampleField field) const noexcept {
    const auto index = static_cast<std::size_t>(field);
    return index < kSampleFieldCount ? values_[index] : 0;
  }

  // Named convenience accessors. Each returns an optional so that a caller
  // cannot accidentally read an unreported field as zero.
  [[nodiscard]] std::optional<std::uint64_t> occupancy_bytes() const noexcept {
    return value(SampleField::OccupancyBytes);
  }
  [[nodiscard]] std::optional<std::uint64_t> occupancy_cells() const noexcept {
    return value(SampleField::OccupancyCells);
  }
  [[nodiscard]] std::optional<std::uint64_t> queue_depth_packets() const noexcept {
    return value(SampleField::QueueDepthPackets);
  }
  [[nodiscard]] std::optional<std::uint64_t> drop_packets() const noexcept {
    return value(SampleField::DropPackets);
  }
  [[nodiscard]] std::optional<std::uint64_t> mark_packets() const noexcept {
    return value(SampleField::MarkPackets);
  }
  [[nodiscard]] std::optional<std::uint64_t> pause_duration_nanos() const noexcept {
    return value(SampleField::PauseDurationNanos);
  }
  [[nodiscard]] std::optional<std::uint64_t> max_depth_packets() const noexcept {
    return value(SampleField::MaxDepthPackets);
  }
  [[nodiscard]] std::optional<std::uint64_t> static_threshold_cells() const noexcept {
    return value(SampleField::StaticThresholdCells);
  }
  [[nodiscard]] std::optional<std::uint64_t> dynamic_threshold_cells() const noexcept {
    return value(SampleField::DynamicThresholdCells);
  }
  [[nodiscard]] std::optional<std::uint64_t> occupancy_for_pressure() const noexcept {
    // Cells are the unit queue thresholds are expressed in; fall back to bytes
    // only when cells were not reported, and record which unit was used.
    if (const auto cells = occupancy_cells(); cells.has_value()) {
      return cells;
    }
    return occupancy_bytes();
  }

  /// Fields the pressure policy requires that were not reported.
  [[nodiscard]] FieldMask missing_required(FieldMask required) const noexcept {
    return required & ~reported;
  }

  [[nodiscard]] std::uint64_t values_at(std::size_t index) const noexcept {
    return index < kSampleFieldCount ? values_[index] : 0;
  }

 private:
  std::uint64_t values_[kSampleFieldCount]{};
};

}  // namespace qobs
