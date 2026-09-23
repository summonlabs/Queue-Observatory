#include "qobs/ingest/Wire.hpp"

#include <array>
#include <limits>
#include <optional>

#include "qobs/core/Checked.hpp"
#include "qobs/core/Json.hpp"
#include "qobs/core/Text.hpp"
#include "qobs/version.hpp"

namespace qobs {
namespace {

constexpr std::size_t kMaxDiagnostics = 64u;

const std::vector<wire::MeasurementKey>& keys_table() {
  static const std::vector<wire::MeasurementKey> kKeys{
      {"occupancy_bytes", SampleField::OccupancyBytes},
      {"occupancy_cells", SampleField::OccupancyCells},
      {"queue_depth_packets", SampleField::QueueDepthPackets},
      {"shared_buffer_cells", SampleField::SharedBufferCells},
      {"headroom_cells", SampleField::HeadroomCells},
      {"reserved_cells", SampleField::ReservedCells},
      {"static_threshold_cells", SampleField::StaticThresholdCells},
      {"dynamic_threshold_cells", SampleField::DynamicThresholdCells},
      {"max_depth_packets", SampleField::MaxDepthPackets},
      {"high_watermark_cells", SampleField::HighWatermarkCells},
      {"enqueue_packets", SampleField::EnqueuePackets},
      {"dequeue_packets", SampleField::DequeuePackets},
      {"drop_packets", SampleField::DropPackets},
      {"drop_bytes", SampleField::DropBytes},
      {"mark_packets", SampleField::MarkPackets},
      {"pause_frames", SampleField::PauseFrames},
      {"pause_duration_nanos", SampleField::PauseDurationNanos},
  };
  return kKeys;
}

std::string recognised_measurement_names() {
  static const std::string kNames = [] {
    std::string out;
    for (const auto& entry : keys_table()) {
      if (!out.empty()) {
        out.append(", ");
      }
      out.append(entry.key);
    }
    return out;
  }();
  return kNames;
}

struct LineContext {
  std::size_t line{0};
};

std::string prefix(const LineContext& context) { return "line " + std::to_string(context.line) + ": "; }

struct ParseResult {
  DecodeStatus status{DecodeStatus::Ok};
  std::string reason{};
  QueueSample sample{};
  bool has_sample{false};
  ClassMetadataTable metadata{};
  bool has_metadata{false};
  std::size_t unknown_keys{0};
};

Result<std::string> require_string(const JsonValue& object, std::string_view key,
                                   std::size_t max_bytes, const LineContext& context) {
  const JsonValue* value = object.find(key);
  if (value == nullptr) {
    return Result<std::string>::failure(ErrorCode::ParseError,
                                        prefix(context) + "required field '" +
                                            std::string(key) + "' is missing");
  }
  if (!value->is_string()) {
    return Result<std::string>::failure(ErrorCode::ParseError,
                                        prefix(context) + "field '" + std::string(key) +
                                            "' must be a string");
  }
  if (value->as_string().size() > max_bytes) {
    return Result<std::string>::failure(ErrorCode::LimitExceeded,
                                        prefix(context) + "field '" + std::string(key) +
                                            "' exceeds the configured field size");
  }
  return value->as_string();
}

Result<std::uint64_t> require_u64(const JsonValue& object, std::string_view key,
                                  const LineContext& context) {
  const JsonValue* value = object.find(key);
  if (value == nullptr) {
    return Result<std::uint64_t>::failure(ErrorCode::ParseError,
                                          prefix(context) + "required field '" +
                                              std::string(key) + "' is missing");
  }
  if (value->is_uint()) {
    return value->as_uint();
  }
  if (value->is_int() && value->as_int() >= 0) {
    return static_cast<std::uint64_t>(value->as_int());
  }
  return Result<std::uint64_t>::failure(ErrorCode::ParseError,
                                        prefix(context) + "field '" + std::string(key) +
                                            "' must be a non-negative integer");
}

Result<std::uint64_t> optional_u64(const JsonValue& object, std::string_view key,
                                   std::uint64_t fallback, const LineContext& context) {
  if (object.find(key) == nullptr) {
    return fallback;
  }
  return require_u64(object, key, context);
}

ParseResult parse_sample_record(const JsonValue& object, const LineContext& context,
                                const IngestLimits& limits) {
  ParseResult result;
  const auto device = require_string(object, "device", limits.max_field_bytes, context);
  const auto port = require_string(object, "port", limits.max_field_bytes, context);
  const auto source = require_string(object, "source", limits.max_field_bytes, context);
  const auto incarnation = require_string(object, "incarnation", limits.max_field_bytes, context);
  const auto domain = require_string(object, "clock_domain", limits.max_field_bytes, context);
  const auto queue_index = require_u64(object, "queue", context);
  const auto generation = require_u64(object, "generation", context);
  const auto sequence = require_u64(object, "sequence", context);
  const auto observed = require_u64(object, "observed_ns", context);
  const std::array<const Error*, 9> errors{&device.error(),     &port.error(),
                                           &source.error(),     &incarnation.error(),
                                           &domain.error(),     &queue_index.error(),
                                           &generation.error(), &sequence.error(),
                                           &observed.error()};
  const std::array<bool, 9> present{device.has_value(),     port.has_value(),
                                    source.has_value(),     incarnation.has_value(),
                                    domain.has_value(),     queue_index.has_value(),
                                    generation.has_value(), sequence.has_value(),
                                    observed.has_value()};
  for (std::size_t index = 0; index < present.size(); ++index) {
    if (!present[index]) {
      result.status = DecodeStatus::Malformed;
      result.reason = errors[index]->message();
      return result;
    }
  }

  const auto parsed_device = DeviceId::create(device.value());
  const auto parsed_port = PortId::create(port.value());
  const auto parsed_source = SourceId::create(source.value());
  const auto parsed_incarnation = IncarnationId::create(incarnation.value());
  const auto parsed_domain = ClockDomainId::create(domain.value());
  if (!parsed_device.has_value() || !parsed_port.has_value() || !parsed_source.has_value() ||
      !parsed_incarnation.has_value() || !parsed_domain.has_value()) {
    result.status = DecodeStatus::Malformed;
    result.reason = prefix(context) +
                    "an identity field is empty, too long, or contains a character that is not "
                    "allowed in an identity";
    return result;
  }
  const auto queue_id = checked::narrow_u<std::uint32_t>(queue_index.value());
  if (!queue_id.has_value()) {
    result.status = DecodeStatus::Malformed;
    result.reason = prefix(context) + "the queue index does not fit in 32 bits";
    return result;
  }
  if (observed.value() > static_cast<std::uint64_t>(std::numeric_limits<Nanos>::max())) {
    result.status = DecodeStatus::Malformed;
    result.reason = prefix(context) + "the observation timestamp is outside the supported range";
    return result;
  }

  QueueSample sample;
  sample.queue.device = parsed_device.value();
  sample.queue.port = parsed_port.value();
  sample.queue.queue = QueueId::from_raw(*queue_id);
  sample.source = parsed_source.value();
  sample.incarnation = parsed_incarnation.value();
  sample.generation = GenerationId::from_raw(generation.value());
  sample.sequence = SourceSequence::from_raw(sequence.value());
  sample.observed.ns = static_cast<Nanos>(observed.value());
  sample.observed.domain = parsed_domain.value();

  if (const JsonValue* authority = object.find("authority"); authority != nullptr) {
    if (!authority->is_string()) {
      result.status = DecodeStatus::Malformed;
      result.reason = prefix(context) + "field 'authority' must be a string";
      return result;
    }
    bool recognised = false;
    sample.authority = parse_source_authority(authority->as_string(), recognised);
    if (!recognised) {
      result.status = DecodeStatus::Unsupported;
      result.reason = prefix(context) + "authority value '" +
                      text::sanitize_for_display(authority->as_string(), 32) +
                      "' is not recognised by this runtime";
      return result;
    }
  }

  if (object.find("revision") != nullptr) {
    const auto parsed_revision = require_u64(object, "revision", context);
    if (!parsed_revision.has_value()) {
      result.status = DecodeStatus::Malformed;
      result.reason = parsed_revision.error().message();
      return result;
    }
    sample.revision = Revision::from_raw(parsed_revision.value());
  }

  if (object.find("traffic_class") != nullptr) {
    const auto parsed_traffic = require_u64(object, "traffic_class", context);
    if (!parsed_traffic.has_value()) {
      result.status = DecodeStatus::Malformed;
      result.reason = parsed_traffic.error().message();
      return result;
    }
    const auto narrow = checked::narrow_u<std::uint16_t>(parsed_traffic.value());
    if (!narrow.has_value()) {
      result.status = DecodeStatus::Malformed;
      result.reason = prefix(context) + "field 'traffic_class' does not fit in 16 bits";
      return result;
    }
    sample.classes.traffic_class = TrafficClassId::from_raw(*narrow);
    sample.classes.origin = AttributeOrigin::ObservedInSample;
  }
  if (const JsonValue* scheduling = object.find("scheduling_class"); scheduling != nullptr) {
    if (!scheduling->is_string()) {
      result.status = DecodeStatus::Malformed;
      result.reason = prefix(context) + "field 'scheduling_class' must be a string";
      return result;
    }
    const auto parsed = SchedulingClassId::create(scheduling->as_string());
    if (!parsed.has_value()) {
      result.status = DecodeStatus::Malformed;
      result.reason = prefix(context) + "field 'scheduling_class' is not a usable identity";
      return result;
    }
    sample.classes.scheduling_class = parsed.value();
    sample.classes.origin = AttributeOrigin::ObservedInSample;
  }

  for (const auto& entry : keys_table()) {
    if (object.find(entry.key) == nullptr) {
      continue;
    }
    const auto parsed_value = require_u64(object, entry.key, context);
    if (!parsed_value.has_value()) {
      result.status = DecodeStatus::Malformed;
      result.reason = parsed_value.error().message();
      return result;
    }
    sample.set_value(entry.field, parsed_value.value());
  }
  if (sample.reported == 0u) {
    result.status = DecodeStatus::Malformed;
    result.reason = prefix(context) +
                    "the record carries no measurement; accepted measurement names are " +
                    recognised_measurement_names();
    return result;
  }

  if (const JsonValue* declared = object.find("declared"); declared != nullptr) {
    if (!declared->is_array()) {
      result.status = DecodeStatus::Malformed;
      result.reason = prefix(context) + "field 'declared' must be an array of measurement names";
      return result;
    }
    for (const JsonValue& name : declared->items()) {
      if (!name.is_string()) {
        result.status = DecodeStatus::Malformed;
        result.reason = prefix(context) + "field 'declared' must contain only strings";
        return result;
      }
      const auto field = parse_sample_field(name.as_string());
      if (!field.has_value()) {
        ++result.unknown_keys;
        continue;
      }
      sample.declared = sample.declared | field_bit(*field);
    }
  } else {
    sample.declared = sample.reported;
  }

  static const std::array<std::string_view, 11> kFrameworkKeys{
      "kind",       "v",        "device",     "port",       "queue",
      "source",     "incarnation", "generation", "sequence", "observed_ns",
      "clock_domain"};
  static const std::array<std::string_view, 5> kOptionalKeys{"declared", "authority", "revision",
                                                             "traffic_class",
                                                             "scheduling_class"};
  for (std::size_t index = 0; index < object.size(); ++index) {
    const std::string_view name = object.key_at(index);
    bool known = false;
    for (const auto& entry : keys_table()) {
      if (entry.key == name) {
        known = true;
        break;
      }
    }
    for (const std::string_view candidate : kFrameworkKeys) {
      if (!known && candidate == name) {
        known = true;
        break;
      }
    }
    for (const std::string_view candidate : kOptionalKeys) {
      if (!known && candidate == name) {
        known = true;
        break;
      }
    }
    if (!known) {
      ++result.unknown_keys;
    }
  }

  sample.declared_version = wire::kVersion;
  result.sample = std::move(sample);
  result.has_sample = true;
  return result;
}

ParseResult parse_metadata_record(const JsonValue& object, const LineContext& context,
                                  const IngestLimits& limits) {
  ParseResult result;
  ClassMetadataTable table;
  const auto revision = require_u64(object, "revision", context);
  const auto generation = require_u64(object, "generation", context);
  const auto source = require_string(object, "source", limits.max_field_bytes, context);
  if (!revision.has_value() || !generation.has_value() || !source.has_value()) {
    result.status = DecodeStatus::Malformed;
    result.reason = !revision.has_value()     ? revision.error().message()
                    : !generation.has_value() ? generation.error().message()
                                              : source.error().message();
    return result;
  }
  table.revision = Revision::from_raw(revision.value());
  table.generation = GenerationId::from_raw(generation.value());
  const auto parsed_source = SourceId::create(source.value());
  if (!parsed_source.has_value()) {
    result.status = DecodeStatus::Malformed;
    result.reason = prefix(context) + "metadata source is not a usable identity";
    return result;
  }
  table.source = parsed_source.value();

  if (const JsonValue* authority = object.find("authority"); authority != nullptr) {
    if (!authority->is_string()) {
      result.status = DecodeStatus::Malformed;
      result.reason = prefix(context) + "field 'authority' must be a string";
      return result;
    }
    bool recognised = false;
    table.authority = parse_source_authority(authority->as_string(), recognised);
    if (!recognised) {
      result.status = DecodeStatus::Unsupported;
      result.reason = prefix(context) + "metadata authority value is not recognised";
      return result;
    }
  }

  if (const JsonValue* traffic_classes = object.find("traffic_classes");
      traffic_classes != nullptr) {
    if (!traffic_classes->is_array()) {
      result.status = DecodeStatus::Malformed;
      result.reason = prefix(context) + "field 'traffic_classes' must be an array";
      return result;
    }
    if (traffic_classes->size() > limits.max_batch_metadata) {
      result.status = DecodeStatus::Malformed;
      result.reason = prefix(context) + "the traffic class section exceeds the configured limit";
      return result;
    }
    for (const JsonValue& entry : traffic_classes->items()) {
      if (!entry.is_object()) {
        result.status = DecodeStatus::Malformed;
        result.reason = prefix(context) + "every traffic class entry must be an object";
        return result;
      }
      const auto id = require_u64(entry, "id", context);
      if (!id.has_value()) {
        result.status = DecodeStatus::Malformed;
        result.reason = id.error().message();
        return result;
      }
      const auto narrow = checked::narrow_u<std::uint16_t>(id.value());
      if (!narrow.has_value()) {
        result.status = DecodeStatus::Malformed;
        result.reason = prefix(context) + "a traffic class identifier does not fit in 16 bits";
        return result;
      }
      TrafficClassDescriptor descriptor;
      descriptor.id = TrafficClassId::from_raw(*narrow);
      if (const JsonValue* name = entry.find("name"); name != nullptr) {
        if (!name->is_string() || name->as_string().size() > limits.max_field_bytes) {
          result.status = DecodeStatus::Malformed;
          result.reason = prefix(context) + "traffic class 'name' must be a bounded string";
          return result;
        }
        descriptor.name = name->as_string();
      }
      if (entry.find("dscp") != nullptr) {
        const auto dscp = require_u64(entry, "dscp", context);
        if (!dscp.has_value()) {
          result.status = DecodeStatus::Malformed;
          result.reason = dscp.error().message();
          return result;
        }
        const auto value = checked::narrow_u<std::uint16_t>(dscp.value());
        if (!value.has_value()) {
          result.status = DecodeStatus::Malformed;
          result.reason = prefix(context) + "DSCP value does not fit in 16 bits";
          return result;
        }
        descriptor.dscp = *value;
      }
      if (entry.find("pcp") != nullptr) {
        const auto pcp = require_u64(entry, "pcp", context);
        if (!pcp.has_value()) {
          result.status = DecodeStatus::Malformed;
          result.reason = pcp.error().message();
          return result;
        }
        const auto value = checked::narrow_u<std::uint16_t>(pcp.value());
        if (!value.has_value()) {
          result.status = DecodeStatus::Malformed;
          result.reason = prefix(context) + "priority code point does not fit in 16 bits";
          return result;
        }
        descriptor.priority_code_point = *value;
      }
      if (const JsonValue* sc = entry.find("scheduling_class"); sc != nullptr) {
        if (!sc->is_string()) {
          result.status = DecodeStatus::Malformed;
          result.reason = prefix(context) + "traffic class 'scheduling_class' must be a string";
          return result;
        }
        const auto parsed = SchedulingClassId::create(sc->as_string());
        if (!parsed.has_value()) {
          result.status = DecodeStatus::Malformed;
          result.reason =
              prefix(context) + "traffic class 'scheduling_class' is not a usable identity";
          return result;
        }
        descriptor.scheduling_class = parsed.value();
      }
      table.traffic_classes.push_back(std::move(descriptor));
    }
  }

  if (const JsonValue* scheduling_classes = object.find("scheduling_classes");
      scheduling_classes != nullptr) {
    if (!scheduling_classes->is_array()) {
      result.status = DecodeStatus::Malformed;
      result.reason = prefix(context) + "field 'scheduling_classes' must be an array";
      return result;
    }
    if (scheduling_classes->size() > limits.max_batch_metadata) {
      result.status = DecodeStatus::Malformed;
      result.reason =
          prefix(context) + "the scheduling class section exceeds the configured limit";
      return result;
    }
    for (const JsonValue& entry : scheduling_classes->items()) {
      if (!entry.is_object()) {
        result.status = DecodeStatus::Malformed;
        result.reason = prefix(context) + "every scheduling class entry must be an object";
        return result;
      }
      const auto id = require_string(entry, "id", limits.max_field_bytes, context);
      if (!id.has_value()) {
        result.status = DecodeStatus::Malformed;
        result.reason = id.error().message();
        return result;
      }
      const auto parsed = SchedulingClassId::create(id.value());
      if (!parsed.has_value()) {
        result.status = DecodeStatus::Malformed;
        result.reason = prefix(context) + "scheduling class 'id' is not a usable identity";
        return result;
      }
      SchedulingClassDescriptor descriptor;
      descriptor.id = parsed.value();
      if (const JsonValue* mode = entry.find("mode"); mode != nullptr) {
        if (!mode->is_string() || mode->as_string().size() > limits.max_field_bytes) {
          result.status = DecodeStatus::Malformed;
          result.reason = prefix(context) + "scheduling class 'mode' must be a bounded string";
          return result;
        }
        descriptor.mode = mode->as_string();
      }
      if (entry.find("weight") != nullptr) {
        const auto weight = require_u64(entry, "weight", context);
        if (!weight.has_value()) {
          result.status = DecodeStatus::Malformed;
          result.reason = weight.error().message();
          return result;
        }
        descriptor.weight = weight.value();
      }
      table.scheduling_classes.push_back(std::move(descriptor));
    }
  }

  if (const JsonValue* bindings = object.find("bindings"); bindings != nullptr) {
    if (!bindings->is_array()) {
      result.status = DecodeStatus::Malformed;
      result.reason = prefix(context) + "field 'bindings' must be an array";
      return result;
    }
    if (bindings->size() > limits.max_batch_metadata) {
      result.status = DecodeStatus::Malformed;
      result.reason = prefix(context) + "the binding section exceeds the configured limit";
      return result;
    }
    for (const JsonValue& entry : bindings->items()) {
      if (!entry.is_object()) {
        result.status = DecodeStatus::Malformed;
        result.reason = prefix(context) + "every binding entry must be an object";
        return result;
      }
      const auto device = require_string(entry, "device", limits.max_field_bytes, context);
      const auto port = require_string(entry, "port", limits.max_field_bytes, context);
      const auto queue = require_u64(entry, "queue", context);
      if (!device.has_value() || !port.has_value() || !queue.has_value()) {
        result.status = DecodeStatus::Malformed;
        result.reason = !device.has_value() ? device.error().message()
                        : !port.has_value() ? port.error().message()
                                            : queue.error().message();
        return result;
      }
      const auto parsed_device = DeviceId::create(device.value());
      const auto parsed_port = PortId::create(port.value());
      const auto queue_id = checked::narrow_u<std::uint32_t>(queue.value());
      if (!parsed_device.has_value() || !parsed_port.has_value() || !queue_id.has_value()) {
        result.status = DecodeStatus::Malformed;
        result.reason = prefix(context) + "a binding names an unusable queue path";
        return result;
      }
      QueueClassBinding binding;
      binding.queue.device = parsed_device.value();
      binding.queue.port = parsed_port.value();
      binding.queue.queue = QueueId::from_raw(*queue_id);
      if (entry.find("traffic_class") != nullptr) {
        const auto traffic = require_u64(entry, "traffic_class", context);
        if (!traffic.has_value()) {
          result.status = DecodeStatus::Malformed;
          result.reason = traffic.error().message();
          return result;
        }
        const auto value = checked::narrow_u<std::uint16_t>(traffic.value());
        if (!value.has_value()) {
          result.status = DecodeStatus::Malformed;
          result.reason = prefix(context) + "a binding traffic class does not fit in 16 bits";
          return result;
        }
        binding.traffic_class = TrafficClassId::from_raw(*value);
      }
      if (const JsonValue* sc = entry.find("scheduling_class"); sc != nullptr) {
        if (!sc->is_string()) {
          result.status = DecodeStatus::Malformed;
          result.reason = prefix(context) + "binding 'scheduling_class' must be a string";
          return result;
        }
        const auto parsed = SchedulingClassId::create(sc->as_string());
        if (!parsed.has_value()) {
          result.status = DecodeStatus::Malformed;
          result.reason = prefix(context) + "binding 'scheduling_class' is not usable";
          return result;
        }
        binding.scheduling_class = parsed.value();
      }
      table.bindings.push_back(std::move(binding));
    }
  }

  if (table.revision.value() == 0u) {
    result.status = DecodeStatus::Malformed;
    result.reason = prefix(context) + "metadata revision must be non-zero";
    return result;
  }
  if (table.generation.value() == 0u) {
    result.status = DecodeStatus::Malformed;
    result.reason = prefix(context) + "metadata generation must be non-zero";
    return result;
  }
  result.metadata = std::move(table);
  result.has_metadata = true;
  return result;
}

void push_diagnostic(DecodeOutcome& outcome, std::size_t line, DecodeStatus status,
                     std::string reason) {
  if (outcome.diagnostics.size() >= kMaxDiagnostics) {
    outcome.diagnostics_truncated = true;
    return;
  }
  DecodeDiagnostic diagnostic;
  diagnostic.line = line;
  diagnostic.status = status;
  diagnostic.reason = std::move(reason);
  outcome.diagnostics.push_back(std::move(diagnostic));
}

}  // namespace

std::string_view to_string(DecodeStatus status) noexcept {
  switch (status) {
    case DecodeStatus::Ok:
      return "ok";
    case DecodeStatus::Skipped:
      return "skipped";
    case DecodeStatus::Malformed:
      return "malformed";
    case DecodeStatus::Unsupported:
      return "unsupported";
    case DecodeStatus::VersionMismatch:
      return "version_mismatch";
  }
  return "malformed";
}

const std::vector<wire::MeasurementKey>& wire::measurement_keys() { return keys_table(); }

Status decode_document(std::string_view payload, const IngestLimits& limits,
                       DecodeOutcome& outcome) {
  outcome = DecodeOutcome{};
  const Status limits_status = validate_limits(limits);
  if (!limits_status.ok()) {
    return limits_status;
  }
  if (payload.size() > limits.max_payload_bytes) {
    return Status::failure(ErrorCode::LimitExceeded,
                           "the document exceeds the configured payload size");
  }

  JsonLimits json_limits;
  json_limits.max_document_bytes = wire::kMaxLineBytes;
  json_limits.max_depth = limits.max_json_depth;
  json_limits.max_nodes = limits.max_json_nodes;
  json_limits.max_string_bytes = limits.max_field_bytes;
  json_limits.max_object_members = 64u;
  json_limits.max_array_elements = limits.max_batch_metadata;

  std::size_t line_number = 0;
  std::size_t cursor = 0;
  while (cursor <= payload.size()) {
    const std::size_t newline = payload.find('\n', cursor);
    const std::size_t end = newline == std::string_view::npos ? payload.size() : newline;
    std::string_view line = payload.substr(cursor, end - cursor);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1u);
    }
    ++line_number;
    cursor = end + 1u;
    const std::string_view trimmed = text::trim(line);
    if (trimmed.empty() || trimmed.front() == '#') {
      ++outcome.skipped;
      continue;
    }
    ++outcome.lines;
    if (trimmed.size() > wire::kMaxLineBytes) {
      ++outcome.malformed;
      push_diagnostic(outcome, line_number, DecodeStatus::Malformed,
                      "line " + std::to_string(line_number) +
                          ": the record exceeds the maximum line size");
      continue;
    }

    const LineContext context{line_number};
    const auto document = parse_json(trimmed, json_limits);
    if (!document.has_value()) {
      ++outcome.malformed;
      push_diagnostic(outcome, line_number, DecodeStatus::Malformed,
                      "line " + std::to_string(line_number) +
                          ": the record is not valid JSON: " + document.error().message());
      continue;
    }
    if (!document.value().is_object()) {
      ++outcome.malformed;
      push_diagnostic(outcome, line_number, DecodeStatus::Malformed,
                      "line " + std::to_string(line_number) +
                          ": every record must be a JSON object");
      continue;
    }
    const JsonValue& object = document.value();

    std::string kind;
    if (const JsonValue* kind_value = object.find("kind"); kind_value != nullptr) {
      if (!kind_value->is_string()) {
        ++outcome.malformed;
        push_diagnostic(outcome, line_number, DecodeStatus::Malformed,
                        prefix(context) + "field 'kind' must be a string");
        continue;
      }
      kind = kind_value->as_string();
    } else {
      ++outcome.malformed;
      push_diagnostic(outcome, line_number, DecodeStatus::Malformed,
                      prefix(context) + "field 'kind' is required");
      continue;
    }

    const auto version_result = optional_u64(object, "v", wire::kVersion, context);
    if (!version_result.has_value()) {
      ++outcome.malformed;
      push_diagnostic(outcome, line_number, DecodeStatus::Malformed,
                      version_result.error().message());
      continue;
    }
    if (version_result.value() != wire::kVersion) {
      ++outcome.version_mismatches;
      push_diagnostic(outcome, line_number, DecodeStatus::VersionMismatch,
                      prefix(context) + "record declares version " +
                          std::to_string(version_result.value()) + " but this runtime reads " +
                          std::to_string(wire::kVersion));
      continue;
    }

    if (kind == "queue_sample") {
      if (outcome.samples.size() >= limits.max_batch_samples) {
        ++outcome.malformed;
        push_diagnostic(outcome, line_number, DecodeStatus::Malformed,
                        prefix(context) + "the document exceeds the configured sample count");
        continue;
      }
      ParseResult parsed = parse_sample_record(object, context, limits);
      if (!parsed.has_sample) {
        switch (parsed.status) {
          case DecodeStatus::Unsupported:
            ++outcome.unsupported;
            break;
          case DecodeStatus::VersionMismatch:
            ++outcome.version_mismatches;
            break;
          default:
            ++outcome.malformed;
            break;
        }
        push_diagnostic(outcome, line_number, parsed.status, std::move(parsed.reason));
        continue;
      }
      outcome.unknown_keys += parsed.unknown_keys;
      outcome.samples.push_back(std::move(parsed.sample));
      ++outcome.sample_count;
      ++outcome.records;
      continue;
    }
    if (kind == "class_metadata") {
      if (outcome.metadata.size() >= limits.max_batch_metadata) {
        ++outcome.malformed;
        push_diagnostic(outcome, line_number, DecodeStatus::Malformed,
                        prefix(context) + "the document exceeds the configured metadata count");
        continue;
      }
      ParseResult parsed = parse_metadata_record(object, context, limits);
      if (!parsed.has_metadata) {
        switch (parsed.status) {
          case DecodeStatus::Unsupported:
            ++outcome.unsupported;
            break;
          default:
            ++outcome.malformed;
            break;
        }
        push_diagnostic(outcome, line_number, parsed.status, std::move(parsed.reason));
        continue;
      }
      outcome.metadata.push_back(std::move(parsed.metadata));
      ++outcome.metadata_count;
      ++outcome.metadata_records;
      ++outcome.records;
      continue;
    }

    ++outcome.unsupported;
    push_diagnostic(outcome, line_number, DecodeStatus::Unsupported,
                    prefix(context) + "record kind '" + text::sanitize_for_display(kind, 32) +
                        "' is not interpreted by this runtime");
  }
  return Status::success();
}

Status validate_encodable_sample(const QueueSample& sample) {
  if (!sample.queue.valid()) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "a canonical record requires a queue path with a device and a port");
  }
  if (!sample.source.valid()) {
    return Status::failure(ErrorCode::InvalidArgument, "a canonical record requires a source name");
  }
  if (!sample.incarnation.valid()) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "a canonical record requires a source incarnation token");
  }
  if (!sample.generation.valid()) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "a canonical record requires a non-zero device generation");
  }
  if (!sample.sequence.valid()) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "a canonical record requires a non-zero source sequence");
  }
  if (!sample.observed.domain.valid()) {
    return Status::failure(
        ErrorCode::InvalidArgument,
        "a canonical record requires the clock domain of its observation timestamp");
  }
  if (sample.reported == 0u) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "a canonical record must carry at least one measurement");
  }
  return Status::success();
}

Status encode_sample(const QueueSample& sample, std::string& out) {
  QOBS_TRY(validate_encodable_sample(sample));
  JsonWriter writer;
  QOBS_TRY(writer.begin_object());
  QOBS_TRY(writer.member_string("kind", "queue_sample"));
  QOBS_TRY(writer.member_u64("v", wire::kVersion));
  QOBS_TRY(writer.member_string("device", sample.queue.device.view()));
  QOBS_TRY(writer.member_string("port", sample.queue.port.view()));
  QOBS_TRY(writer.member_u64("queue", sample.queue.queue.value()));
  QOBS_TRY(writer.member_string("source", sample.source.view()));
  QOBS_TRY(writer.member_string("incarnation", sample.incarnation.view()));
  QOBS_TRY(writer.member_u64("generation", sample.generation.value()));
  QOBS_TRY(writer.member_u64("sequence", sample.sequence.value()));
  QOBS_TRY(writer.member_string("authority", to_string(sample.authority)));
  QOBS_TRY(writer.member_i64("observed_ns", sample.observed.ns));
  QOBS_TRY(writer.member_string("clock_domain", sample.observed.domain.view()));
  if (sample.revision.valid()) {
    QOBS_TRY(writer.member_u64("revision", sample.revision.value()));
  }
  if (sample.classes.traffic_class.has_value()) {
    QOBS_TRY(writer.member_u64("traffic_class", sample.classes.traffic_class->value()));
  }
  if (sample.classes.scheduling_class.has_value()) {
    QOBS_TRY(writer.member_string("scheduling_class", sample.classes.scheduling_class->view()));
  }
  for (const auto& entry : keys_table()) {
    if (!sample.has(entry.field)) {
      continue;
    }
    QOBS_TRY(writer.member_u64(entry.key, sample.raw_value_or_zero(entry.field)));
  }
  QOBS_TRY(writer.key("declared"));
  QOBS_TRY(writer.begin_array());
  for (const auto& entry : keys_table()) {
    if (!has_field(sample.declared, entry.field)) {
      continue;
    }
    QOBS_TRY(writer.value_string(entry.key));
  }
  QOBS_TRY(writer.end_array());
  QOBS_TRY(writer.end_object());
  out = writer.take();
  return Status::success();
}

Status encode_metadata(const ClassMetadataTable& table, std::string& out) {
  JsonWriter writer;
  QOBS_TRY(writer.begin_object());
  QOBS_TRY(writer.member_string("kind", "class_metadata"));
  QOBS_TRY(writer.member_u64("v", wire::kVersion));
  QOBS_TRY(writer.member_u64("revision", table.revision.value()));
  QOBS_TRY(writer.member_u64("generation", table.generation.value()));
  QOBS_TRY(writer.member_string("source", table.source.view()));
  QOBS_TRY(writer.member_string("authority", to_string(table.authority)));
  QOBS_TRY(writer.key("traffic_classes"));
  QOBS_TRY(writer.begin_array());
  for (const TrafficClassDescriptor& descriptor : table.traffic_classes) {
    QOBS_TRY(writer.begin_object());
    QOBS_TRY(writer.member_u64("id", descriptor.id.value()));
    QOBS_TRY(writer.member_string("name", descriptor.name));
    if (descriptor.dscp.has_value()) {
      QOBS_TRY(writer.member_u64("dscp", *descriptor.dscp));
    }
    if (descriptor.priority_code_point.has_value()) {
      QOBS_TRY(writer.member_u64("pcp", *descriptor.priority_code_point));
    }
    if (descriptor.scheduling_class.has_value()) {
      QOBS_TRY(writer.member_string("scheduling_class", descriptor.scheduling_class->view()));
    }
    QOBS_TRY(writer.end_object());
  }
  QOBS_TRY(writer.end_array());
  QOBS_TRY(writer.key("scheduling_classes"));
  QOBS_TRY(writer.begin_array());
  for (const SchedulingClassDescriptor& descriptor : table.scheduling_classes) {
    QOBS_TRY(writer.begin_object());
    QOBS_TRY(writer.member_string("id", descriptor.id.view()));
    QOBS_TRY(writer.member_string("mode", descriptor.mode));
    if (descriptor.weight.has_value()) {
      QOBS_TRY(writer.member_u64("weight", *descriptor.weight));
    }
    QOBS_TRY(writer.end_object());
  }
  QOBS_TRY(writer.end_array());
  QOBS_TRY(writer.key("bindings"));
  QOBS_TRY(writer.begin_array());
  for (const QueueClassBinding& binding : table.bindings) {
    QOBS_TRY(writer.begin_object());
    QOBS_TRY(writer.member_string("device", binding.queue.device.view()));
    QOBS_TRY(writer.member_string("port", binding.queue.port.view()));
    QOBS_TRY(writer.member_u64("queue", binding.queue.queue.value()));
    if (binding.traffic_class.has_value()) {
      QOBS_TRY(writer.member_u64("traffic_class", binding.traffic_class->value()));
    }
    if (binding.scheduling_class.has_value()) {
      QOBS_TRY(writer.member_string("scheduling_class", binding.scheduling_class->view()));
    }
    QOBS_TRY(writer.end_object());
  }
  QOBS_TRY(writer.end_array());
  QOBS_TRY(writer.end_object());
  out = writer.take();
  return Status::success();
}

}  // namespace qobs
