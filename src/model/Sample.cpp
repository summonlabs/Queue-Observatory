#include "qobs/model/Sample.hpp"

#include <array>

namespace qobs {
namespace {

struct FieldName {
  SampleField field;
  std::string_view name;
};

constexpr std::array<FieldName, kSampleFieldCount> kFieldNames{{
    {SampleField::OccupancyBytes, "occupancy_bytes"},
    {SampleField::OccupancyCells, "occupancy_cells"},
    {SampleField::QueueDepthPackets, "queue_depth_packets"},
    {SampleField::SharedBufferCells, "shared_buffer_cells"},
    {SampleField::HeadroomCells, "headroom_cells"},
    {SampleField::ReservedCells, "reserved_cells"},
    {SampleField::StaticThresholdCells, "static_threshold_cells"},
    {SampleField::DynamicThresholdCells, "dynamic_threshold_cells"},
    {SampleField::MaxDepthPackets, "max_depth_packets"},
    {SampleField::HighWatermarkCells, "high_watermark_cells"},
    {SampleField::EnqueuePackets, "enqueue_packets"},
    {SampleField::DequeuePackets, "dequeue_packets"},
    {SampleField::DropPackets, "drop_packets"},
    {SampleField::DropBytes, "drop_bytes"},
    {SampleField::MarkPackets, "mark_packets"},
    {SampleField::PauseFrames, "pause_frames"},
    {SampleField::PauseDurationNanos, "pause_duration_nanos"},
}};

}  // namespace

std::string_view to_string(SampleField field) noexcept {
  const auto index = static_cast<std::size_t>(field);
  if (index >= kFieldNames.size()) {
    return "unknown";
  }
  return kFieldNames[index].name;
}

std::optional<SampleField> parse_sample_field(std::string_view text) noexcept {
  for (const auto& entry : kFieldNames) {
    if (entry.name == text) {
      return entry.field;
    }
  }
  return std::nullopt;
}

std::string_view to_string(AttributeOrigin value) noexcept {
  switch (value) {
    case AttributeOrigin::Unspecified:
      return "unspecified";
    case AttributeOrigin::ObservedInSample:
      return "observed_in_sample";
    case AttributeOrigin::ResolvedFromMetadata:
      return "resolved_from_metadata";
  }
  return "unknown";
}

}  // namespace qobs
