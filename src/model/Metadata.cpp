#include "qobs/model/Metadata.hpp"

namespace qobs {

const TrafficClassDescriptor* ClassMetadataTable::find_traffic_class(TrafficClassId id) const noexcept {
  for (const auto& entry : traffic_classes) {
    if (entry.id == id) {
      return &entry;
    }
  }
  return nullptr;
}

const SchedulingClassDescriptor* ClassMetadataTable::find_scheduling_class(
    SchedulingClassId id) const noexcept {
  for (const auto& entry : scheduling_classes) {
    if (entry.id == id) {
      return &entry;
    }
  }
  return nullptr;
}

const QueueClassBinding* ClassMetadataTable::find_binding(const QueuePath& queue) const noexcept {
  for (const auto& entry : bindings) {
    if (entry.queue == queue) {
      return &entry;
    }
  }
  return nullptr;
}

MetadataResolution resolve_class_attributes(const ClassMetadataTable& table, const QueuePath& queue,
                                            const QueueClassAttributes& observed) {
  MetadataResolution resolution;
  resolution.attributes = observed;

  const QueueClassBinding* binding = table.find_binding(queue);
  if (binding != nullptr) {
    if (!resolution.attributes.traffic_class.has_value() && binding->traffic_class.has_value()) {
      resolution.attributes.traffic_class = binding->traffic_class;
      resolution.used_metadata = true;
    }
    if (!resolution.attributes.scheduling_class.has_value() &&
        binding->scheduling_class.has_value()) {
      resolution.attributes.scheduling_class = binding->scheduling_class;
      resolution.used_metadata = true;
    }
  }

  // A traffic class descriptor may itself name the scheduling class, which is
  // the only way to correlate a queue that carries no explicit binding.
  if (!resolution.attributes.scheduling_class.has_value() &&
      resolution.attributes.traffic_class.has_value()) {
    const TrafficClassDescriptor* descriptor =
        table.find_traffic_class(*resolution.attributes.traffic_class);
    if (descriptor != nullptr && descriptor->scheduling_class.has_value()) {
      resolution.attributes.scheduling_class = descriptor->scheduling_class;
      resolution.used_metadata = true;
    }
  }

  if (resolution.used_metadata) {
    resolution.attributes.origin = AttributeOrigin::ResolvedFromMetadata;
    resolution.attributes.metadata_revision = table.revision;
  } else if (observed.traffic_class.has_value() || observed.scheduling_class.has_value()) {
    resolution.attributes.origin = AttributeOrigin::ObservedInSample;
  }

  // Report references to classes the table does not describe. The attributes
  // are still returned, because hiding them would lose evidence, but the caller
  // marks the record unsupported for class-level correlation.
  if (resolution.attributes.traffic_class.has_value() &&
      table.find_traffic_class(*resolution.attributes.traffic_class) == nullptr) {
    resolution.referenced_unknown_class = true;
  }
  if (resolution.attributes.scheduling_class.has_value() &&
      table.find_scheduling_class(*resolution.attributes.scheduling_class) == nullptr) {
    resolution.referenced_unknown_class = true;
  }
  return resolution;
}

Status validate_metadata(const ClassMetadataTable& table, std::size_t max_entries_per_section) {
  if (!table.revision.valid()) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "metadata revision must be a non-zero revision number");
  }
  if (!table.generation.valid()) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "metadata generation must be a non-zero generation number");
  }
  if (!table.source.valid()) {
    return Status::failure(ErrorCode::InvalidArgument, "metadata source must be named");
  }
  if (table.traffic_classes.size() > max_entries_per_section) {
    return Status::failure(ErrorCode::LimitExceeded,
                           "metadata traffic class section exceeds the configured limit");
  }
  if (table.scheduling_classes.size() > max_entries_per_section) {
    return Status::failure(ErrorCode::LimitExceeded,
                           "metadata scheduling class section exceeds the configured limit");
  }
  if (table.bindings.size() > max_entries_per_section) {
    return Status::failure(ErrorCode::LimitExceeded,
                           "metadata binding section exceeds the configured limit");
  }
  for (std::size_t index = 0; index < table.traffic_classes.size(); ++index) {
    if (!table.traffic_classes[index].id.valid()) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "metadata traffic class entries require a non-zero identifier");
    }
    for (std::size_t other = index + 1u; other < table.traffic_classes.size(); ++other) {
      if (table.traffic_classes[index].id == table.traffic_classes[other].id) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "metadata contains a duplicate traffic class identifier");
      }
    }
  }
  for (std::size_t index = 0; index < table.scheduling_classes.size(); ++index) {
    if (!table.scheduling_classes[index].id.valid()) {
      return Status::failure(
          ErrorCode::InvalidArgument,
          "metadata scheduling class entries require a non-empty identifier");
    }
    for (std::size_t other = index + 1u; other < table.scheduling_classes.size(); ++other) {
      if (table.scheduling_classes[index].id == table.scheduling_classes[other].id) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "metadata contains a duplicate scheduling class identifier");
      }
    }
  }
  for (std::size_t index = 0; index < table.bindings.size(); ++index) {
    if (!table.bindings[index].queue.valid()) {
      return Status::failure(ErrorCode::InvalidArgument,
                             "metadata bindings require a queue path with a device and a port");
    }
    for (std::size_t other = index + 1u; other < table.bindings.size(); ++other) {
      if (table.bindings[index].queue == table.bindings[other].queue) {
        return Status::failure(ErrorCode::InvalidArgument,
                               "metadata contains a duplicate queue binding");
      }
    }
  }
  return Status::success();
}

}  // namespace qobs
