#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "qobs/core/Result.hpp"
#include "qobs/core/StrongId.hpp"
#include "qobs/core/Time.hpp"
#include "qobs/model/Evidence.hpp"
#include "qobs/model/Identity.hpp"
#include "qobs/model/Sample.hpp"

namespace qobs {

/// A traffic class descriptor. Fields beyond the identifier are optional
/// because a vendor-neutral runtime must be able to represent "this class
/// exists but its mapping was not disclosed".
struct TrafficClassDescriptor {
  TrafficClassId id{};
  std::string name{};
  std::optional<std::uint16_t> dscp{};
  std::optional<std::uint16_t> priority_code_point{};
  std::optional<SchedulingClassId> scheduling_class{};
};

/// A scheduling class descriptor.
struct SchedulingClassDescriptor {
  SchedulingClassId id{};
  /// Free-form but bounded mode label, for example "strict-priority" or
  /// "weighted-round-robin". The runtime does not interpret it beyond equality.
  std::string mode{};
  std::optional<std::uint64_t> weight{};
};

/// An explicit binding of one queue to its classes.
struct QueueClassBinding {
  QueuePath queue{};
  std::optional<TrafficClassId> traffic_class{};
  std::optional<SchedulingClassId> scheduling_class{};
};

/// A complete, versioned snapshot of class metadata for one device generation.
struct ClassMetadataTable {
  Revision revision{};
  GenerationId generation{};
  SourceId source{};
  SourceAuthority authority{SourceAuthority::Unknown};
  ReceiveTime received{};
  std::vector<TrafficClassDescriptor> traffic_classes{};
  std::vector<SchedulingClassDescriptor> scheduling_classes{};
  std::vector<QueueClassBinding> bindings{};

  [[nodiscard]] bool valid() const noexcept { return revision.valid() && generation.valid(); }

  [[nodiscard]] const TrafficClassDescriptor* find_traffic_class(TrafficClassId id) const noexcept;
  [[nodiscard]] const SchedulingClassDescriptor* find_scheduling_class(
      SchedulingClassId id) const noexcept;
  [[nodiscard]] const QueueClassBinding* find_binding(const QueuePath& queue) const noexcept;
};

/// Outcome of resolving a queue's attributes against a metadata table.
struct MetadataResolution {
  QueueClassAttributes attributes{};
  /// True when at least one attribute came from the table rather than the
  /// sample. The caller uses this to record DerivedFromMetadata provenance.
  bool used_metadata{false};
  /// True when the binding named class identifiers that the table does not
  /// describe. The attributes are still reported, but as unsupported detail.
  bool referenced_unknown_class{false};
};

/// Resolve attributes for a queue.
///
/// Values observed in the sample always win. Metadata only fills gaps, and the
/// caller records that it did so.
[[nodiscard]] MetadataResolution resolve_class_attributes(const ClassMetadataTable& table,
                                                          const QueuePath& queue,
                                                          const QueueClassAttributes& observed);

/// Validate a metadata table against bounded limits. Returns the first problem.
[[nodiscard]] Status validate_metadata(const ClassMetadataTable& table,
                                       std::size_t max_entries_per_section);

}  // namespace qobs
