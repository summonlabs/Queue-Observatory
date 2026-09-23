#include "qobs/persist/Format.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <utility>

#include "qobs/core/Checked.hpp"
#include "qobs/core/Crc32c.hpp"
#include "qobs/core/Text.hpp"

namespace qobs {
namespace {

enum class RecordType : std::uint8_t {
  SessionHeader = 0x01,
  QueueSnapshot = 0x02,
  EventBatch = 0x03,
  SourceRegistry = 0x04,
  MetadataTable = 0x05,
};

class ByteWriter {
 public:
  ByteWriter(std::vector<std::byte>& out, std::uint64_t max_bytes)
      : out_(out), max_bytes_(max_bytes) {}

  void set_limit(std::uint64_t max_bytes) noexcept { max_bytes_ = max_bytes; }

  bool u8(std::uint8_t value) { return raw(&value, 1u); }
  bool u16(std::uint16_t value) {
    const std::array<std::byte, 2> bytes{static_cast<std::byte>(value & 0xFFu),
                                         static_cast<std::byte>((value >> 8u) & 0xFFu)};
    return raw(bytes.data(), bytes.size());
  }
  bool u32(std::uint32_t value) {
    std::array<std::byte, 4> bytes{};
    for (std::size_t index = 0; index < 4u; ++index) {
      bytes[index] = static_cast<std::byte>((value >> (8u * index)) & 0xFFu);
    }
    return raw(bytes.data(), bytes.size());
  }
  bool u64(std::uint64_t value) {
    std::array<std::byte, 8> bytes{};
    for (std::size_t index = 0; index < 8u; ++index) {
      bytes[index] = static_cast<std::byte>((value >> (8u * index)) & 0xFFu);
    }
    return raw(bytes.data(), bytes.size());
  }
  bool i64(std::int64_t value) { return u64(static_cast<std::uint64_t>(value)); }
  bool raw(const void* data, std::size_t size) {
    if (!ok_) {
      return false;
    }
    const auto current = static_cast<std::uint64_t>(out_.size());
    const auto next = checked::add_u64(current, static_cast<std::uint64_t>(size));
    if (!next.has_value() || *next > max_bytes_) {
      ok_ = false;
      return false;
    }
    const auto* first = static_cast<const std::byte*>(data);
    out_.insert(out_.end(), first, first + size);
    return true;
  }
  bool string(const std::string& value, std::uint32_t max_length) {
    if (value.size() > max_length) {
      ok_ = false;
      return false;
    }
    if (!u32(static_cast<std::uint32_t>(value.size()))) {
      return false;
    }
    return raw(value.data(), value.size());
  }
  bool name_id(const std::string& value) { return string(value, 128u); }
  bool sample_bitmap(std::uint64_t reported) {
    std::uint8_t count = 0;
    for (std::size_t index = 0; index < kSampleFieldCount; ++index) {
      if (has_field(reported, static_cast<SampleField>(index))) {
        ++count;
      }
    }
    if (!u8(count)) {
      return false;
    }
    for (std::size_t index = 0; index < kSampleFieldCount; ++index) {
      const auto field = static_cast<SampleField>(index);
      if (!has_field(reported, field)) {
        continue;
      }
      if (!u8(static_cast<std::uint8_t>(index))) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] std::uint64_t size() const noexcept {
    return static_cast<std::uint64_t>(out_.size());
  }

 private:
  std::vector<std::byte>& out_;
  std::uint64_t max_bytes_{0};
  bool ok_{true};
};

class ByteReader {
 public:
  ByteReader(std::span<const std::byte> in, std::uint64_t max_field_bytes)
      : in_(in), max_field_bytes_(max_field_bytes) {}

  bool u8(std::uint8_t& value) {
    if (!ensure(1u)) {
      return false;
    }
    value = std::to_integer<std::uint8_t>(in_[offset_]);
    offset_ += 1u;
    return true;
  }
  bool u16(std::uint16_t& value) {
    if (!ensure(2u)) {
      return false;
    }
    value = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(in_[offset_]) |
                                       (std::to_integer<std::uint8_t>(in_[offset_ + 1u]) << 8u));
    offset_ += 2u;
    return true;
  }
  bool u32(std::uint32_t& value) {
    if (!ensure(4u)) {
      return false;
    }
    std::uint32_t accumulator = 0;
    for (std::size_t index = 0; index < 4u; ++index) {
      accumulator |= static_cast<std::uint32_t>(
                         std::to_integer<std::uint8_t>(in_[offset_ + index]))
                     << (8u * index);
    }
    offset_ += 4u;
    value = accumulator;
    return true;
  }
  bool u64(std::uint64_t& value) {
    if (!ensure(8u)) {
      return false;
    }
    std::uint64_t accumulator = 0;
    for (std::size_t index = 0; index < 8u; ++index) {
      accumulator |= static_cast<std::uint64_t>(
                         std::to_integer<std::uint8_t>(in_[offset_ + index]))
                     << (8u * index);
    }
    offset_ += 8u;
    value = accumulator;
    return true;
  }
  bool i64(std::int64_t& value) {
    std::uint64_t raw = 0;
    if (!u64(raw)) {
      return false;
    }
    value = static_cast<std::int64_t>(raw);
    return true;
  }
  bool string(std::string& value, std::uint32_t max_length) {
    std::uint32_t length = 0;
    if (!u32(length)) {
      return false;
    }
    if (length > max_length || length > max_field_bytes_) {
      fail_ = true;
      return false;
    }
    if (!ensure(length)) {
      return false;
    }
    value.assign(reinterpret_cast<const char*>(in_.data() + offset_), length);
    offset_ += length;
    return true;
  }
  bool name_id(std::string& value) { return string(value, 128u); }
  bool sample_bitmap(std::uint64_t& reported) {
    reported = 0;
    std::uint8_t count = 0;
    if (!u8(count)) {
      return false;
    }
    for (std::uint8_t index = 0; index < count; ++index) {
      std::uint8_t field = 0;
      if (!u8(field)) {
        return false;
      }
      if (field >= kSampleFieldCount) {
        fail_ = true;
        return false;
      }
      reported |= field_bit(static_cast<SampleField>(field));
    }
    return true;
  }

  [[nodiscard]] bool ok() const noexcept { return !fail_; }
  [[nodiscard]] std::uint64_t offset() const noexcept { return offset_; }
  [[nodiscard]] std::uint64_t remaining() const noexcept {
    return static_cast<std::uint64_t>(in_.size()) - offset_;
  }

 private:
  bool ensure(std::uint64_t count) {
    if (fail_) {
      return false;
    }
    const auto needed = checked::add_u64(offset_, count);
    if (!needed.has_value() || *needed > static_cast<std::uint64_t>(in_.size())) {
      fail_ = true;
      return false;
    }
    return true;
  }

  std::span<const std::byte> in_{};
  std::uint64_t offset_{0};
  std::uint64_t max_field_bytes_{0};
  bool fail_{false};
};

/// Build the fixed segment header and store the header CRC in place.
///
/// Returns false when the writer could not lay out every field, which would
/// otherwise truncate the header silently and cost the payload checksum.
[[nodiscard]] bool write_header(std::vector<std::byte>& out, std::uint32_t policy_version,
                                std::uint32_t runtime_id_len, std::int64_t created_wall_ns,
                                std::int64_t created_steady_ns, std::uint64_t record_count,
                                std::uint64_t payload_bytes, std::uint32_t payload_crc) {
  std::vector<std::byte> header;
  header.reserve(kSegmentHeaderSize);
  ByteWriter writer(header, kSegmentHeaderSize);
  writer.raw(kSegmentMagic, sizeof(kSegmentMagic));
  writer.u32(QOBS_PERSISTENCE_FORMAT_VERSION);
  writer.u32(kSegmentHeaderSize);
  writer.u32(0u);  // header CRC placeholder, patched below
  writer.u32(policy_version);
  writer.u32(runtime_id_len);
  writer.u32(0u);  // reserved flags
  writer.i64(created_wall_ns);
  writer.i64(created_steady_ns);
  writer.u64(record_count);
  writer.u64(payload_bytes);
  writer.u32(payload_crc);
  writer.u32(QOBS_VERSION_MAJOR);
  writer.u32(QOBS_VERSION_MINOR);
  writer.u32(QOBS_VERSION_PATCH);
  if (!writer.ok() || header.size() != kSegmentHeaderSize) {
    return false;
  }
  const std::uint32_t crc = crc32c(header);
  header[16] = static_cast<std::byte>(crc & 0xFFu);
  header[17] = static_cast<std::byte>((crc >> 8u) & 0xFFu);
  header[18] = static_cast<std::byte>((crc >> 16u) & 0xFFu);
  header[19] = static_cast<std::byte>((crc >> 24u) & 0xFFu);
  out = std::move(header);
  return true;
}

/// Serialize the fixed eight bytes that follow the record checksum: the type,
/// a flags byte, two reserved bytes and the little-endian value length.
std::array<std::byte, 8> record_prefix(RecordType type, std::uint8_t flags, std::uint32_t length) {
  std::array<std::byte, 8> prefix{};
  prefix[0] = static_cast<std::byte>(static_cast<std::uint8_t>(type));
  prefix[1] = static_cast<std::byte>(flags);
  prefix[2] = std::byte{0};
  prefix[3] = std::byte{0};
  for (std::size_t index = 0; index < 4u; ++index) {
    prefix[4u + index] = static_cast<std::byte>((length >> (8u * index)) & 0xFFu);
  }
  return prefix;
}

/// A record is a four-byte checksum followed by an eight-byte prefix and the
/// value. The checksum covers the prefix and the value, so a damaged length
/// field is detected as well as a damaged payload.
bool append_record(std::vector<std::byte>& payload, RecordType type,
                   const std::vector<std::byte>& value, std::uint64_t max_bytes) {
  if (value.size() > max_bytes ||
      value.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    return false;
  }
  const std::array<std::byte, 8> prefix =
      record_prefix(type, 0u, static_cast<std::uint32_t>(value.size()));
  Crc32c crc;
  crc.update(std::span<const std::byte>(prefix.data(), prefix.size()));
  crc.update(value);
  ByteWriter writer(payload, max_bytes);
  writer.u32(crc.value());
  writer.raw(prefix.data(), prefix.size());
  writer.raw(value.data(), value.size());
  return writer.ok();
}

}  // namespace

namespace {

bool encode_queue_record(const DurableQueueState& queue, const PersistenceLimits& limits,
                         std::vector<std::byte>& value) {
  ByteWriter writer(value, limits.max_record_bytes);
  writer.name_id(queue.queue.device.value());
  writer.name_id(queue.queue.port.value());
  writer.u32(queue.queue.queue.value());
  writer.u8(queue.classes_known ? 1u : 0u);
  writer.u8(queue.classes.traffic_class.has_value() ? 1u : 0u);
  if (queue.classes.traffic_class.has_value()) {
    writer.u16(queue.classes.traffic_class->value());
  }
  writer.u8(queue.classes.scheduling_class.has_value() ? 1u : 0u);
  if (queue.classes.scheduling_class.has_value()) {
    writer.name_id(queue.classes.scheduling_class->value());
  }
  writer.u8(static_cast<std::uint8_t>(queue.classes.origin));
  writer.u64(queue.classes.metadata_revision.value());
  if (queue.samples.size() > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  writer.u32(static_cast<std::uint32_t>(queue.samples.size()));
  for (const DurableSample& sample : queue.samples) {
    writer.i64(sample.observed.ns);
    writer.name_id(sample.observed.domain.value());
    writer.i64(sample.received_wall.ns);
    writer.i64(sample.received_steady.ns);
    writer.name_id(sample.source.value());
    writer.name_id(sample.incarnation.value());
    writer.u64(sample.generation.value());
    writer.u64(sample.sequence.value());
    writer.u8(static_cast<std::uint8_t>(sample.authority));
    writer.u64(sample.reported);
    if (!writer.sample_bitmap(sample.reported)) {
      return false;
    }
    for (std::size_t index = 0; index < kSampleFieldCount; ++index) {
      const auto field = static_cast<SampleField>(index);
      if (has_field(sample.reported, field)) {
        writer.u64(sample.values[index]);
      }
    }
    writer.u8(static_cast<std::uint8_t>(sample.state));
    writer.u8(static_cast<std::uint8_t>(sample.freshness));
    writer.u8(static_cast<std::uint8_t>(sample.quality));
    writer.i64(sample.age_ns);
  }
  return writer.ok();
}

}  // namespace

Status encode_snapshot(const DurableSnapshot& snapshot, const PersistenceLimits& limits,
                       std::vector<std::byte>& out) {
  const Status limits_status = validate_limits(limits);
  if (!limits_status.ok()) {
    return limits_status;
  }
  if (snapshot.runtime_id.size() > kMaxRuntimeIdBytes) {
    return Status::failure(ErrorCode::LimitExceeded,
                           "runtime identifier exceeds the maximum persisted size");
  }

  std::vector<std::byte> payload;
  std::uint64_t record_count = 0;

  const auto append = [&](RecordType type, const std::vector<std::byte>& value) -> Status {
    if (static_cast<std::uint64_t>(value.size()) > limits.max_record_bytes) {
      return Status::failure(ErrorCode::LimitExceeded,
                             "a persistence record exceeds the configured record size");
    }
    if (record_count >= limits.max_segment_records) {
      return Status::failure(ErrorCode::LimitExceeded,
                             "the segment would exceed the configured record count");
    }
    if (!append_record(payload, type, value, limits.max_segment_bytes)) {
      return Status::failure(ErrorCode::LimitExceeded,
                             "the segment would exceed the configured byte size");
    }
    ++record_count;
    return Status::success();
  };

  {
    std::vector<std::byte> value;
    ByteWriter writer(value, limits.max_record_bytes);
    writer.string(snapshot.runtime_id, static_cast<std::uint32_t>(kMaxRuntimeIdBytes));
    writer.u32(snapshot.policy_version);
    writer.string(snapshot.policy_name, 128u);
    writer.i64(snapshot.created.wall.ns);
    writer.i64(snapshot.created.steady.ns);
    writer.u32(static_cast<std::uint32_t>(snapshot.queues.size() > 0xFFFFFFFFu
                                              ? 0xFFFFFFFFu
                                              : snapshot.queues.size()));
    if (!writer.ok()) {
      return Status::failure(ErrorCode::Internal, "failed to encode the session header record");
    }
    QOBS_TRY(append(RecordType::SessionHeader, value));
  }

  for (const DurableQueueState& queue : snapshot.queues) {
    std::vector<std::byte> value;
    if (!encode_queue_record(queue, limits, value)) {
      return Status::failure(ErrorCode::LimitExceeded,
                             "a queue snapshot exceeds the configured record size");
    }
    QOBS_TRY(append(RecordType::QueueSnapshot, value));
  }

  if (!snapshot.sources.empty()) {
    std::vector<std::byte> value;
    ByteWriter writer(value, limits.max_record_bytes);
    writer.u32(static_cast<std::uint32_t>(snapshot.sources.size()));
    for (const SourceRecord& source : snapshot.sources) {
      writer.name_id(source.id.value());
      writer.u8(static_cast<std::uint8_t>(source.authority));
      writer.u8(static_cast<std::uint8_t>(source.declared_authority));
      writer.name_id(source.incarnation.value());
      writer.u64(source.incarnation_ordinal);
      writer.u64(source.last_sequence.value());
      writer.u64(source.generation.value());
      writer.name_id(source.clock_domain.value());
      writer.i64(source.first_seen.wall.ns);
      writer.i64(source.first_seen.steady.ns);
      writer.i64(source.last_seen.wall.ns);
      writer.i64(source.last_seen.steady.ns);
      writer.u64(source.accepted);
      writer.u64(source.fenced);
      writer.u64(source.duplicates);
      writer.u64(source.rejected);
      writer.i64(static_cast<std::int64_t>(source.last_observed_ns));
      writer.u32(static_cast<std::uint32_t>(source.recent_incarnations.size()));
      for (const auto& entry : source.recent_incarnations) {
        writer.name_id(entry.first.value());
        writer.u64(entry.second);
      }
    }
    if (!writer.ok()) {
      return Status::failure(ErrorCode::LimitExceeded,
                             "the source registry exceeds the configured record size");
    }
    QOBS_TRY(append(RecordType::SourceRegistry, value));
  }

  if (!snapshot.events.empty()) {
    std::vector<std::byte> value;
    ByteWriter writer(value, limits.max_record_bytes);
    writer.u32(static_cast<std::uint32_t>(snapshot.events.size()));
    for (const EventRecord& event : snapshot.events) {
      writer.u8(static_cast<std::uint8_t>(event.kind));
      writer.u8(static_cast<std::uint8_t>(event.severity));
      writer.i64(event.received.wall.ns);
      writer.i64(event.received.steady.ns);
      writer.i64(event.observed.ns);
      writer.name_id(event.observed.domain.value());
      writer.name_id(event.queue.device.value());
      writer.name_id(event.queue.port.value());
      writer.u32(event.queue.queue.value());
      writer.name_id(event.source.value());
      writer.name_id(event.incarnation.value());
      writer.u64(event.sequence.value());
      writer.u64(event.revision.value());
      writer.string(event.detail, 512u);
    }
    if (!writer.ok()) {
      return Status::failure(ErrorCode::LimitExceeded,
                             "the event batch exceeds the configured record size");
    }
    QOBS_TRY(append(RecordType::EventBatch, value));
  }

  if (snapshot.has_metadata) {
    std::vector<std::byte> value;
    ByteWriter writer(value, limits.max_metadata_bytes);
    writer.u64(snapshot.metadata.revision.value());
    writer.u64(snapshot.metadata.generation.value());
    writer.name_id(snapshot.metadata.source.value());
    writer.u8(static_cast<std::uint8_t>(snapshot.metadata.authority));
    writer.i64(snapshot.metadata.received.wall.ns);
    writer.i64(snapshot.metadata.received.steady.ns);
    writer.u32(static_cast<std::uint32_t>(snapshot.metadata.traffic_classes.size()));
    for (const TrafficClassDescriptor& descriptor : snapshot.metadata.traffic_classes) {
      writer.u16(descriptor.id.value());
      writer.string(descriptor.name, 128u);
      writer.u8(descriptor.dscp.has_value() ? 1u : 0u);
      if (descriptor.dscp.has_value()) {
        writer.u16(*descriptor.dscp);
      }
      writer.u8(descriptor.priority_code_point.has_value() ? 1u : 0u);
      if (descriptor.priority_code_point.has_value()) {
        writer.u16(*descriptor.priority_code_point);
      }
      writer.u8(descriptor.scheduling_class.has_value() ? 1u : 0u);
      if (descriptor.scheduling_class.has_value()) {
        writer.name_id(descriptor.scheduling_class->value());
      }
    }
    writer.u32(static_cast<std::uint32_t>(snapshot.metadata.scheduling_classes.size()));
    for (const SchedulingClassDescriptor& descriptor : snapshot.metadata.scheduling_classes) {
      writer.name_id(descriptor.id.value());
      writer.string(descriptor.mode, 64u);
      writer.u8(descriptor.weight.has_value() ? 1u : 0u);
      if (descriptor.weight.has_value()) {
        writer.u64(*descriptor.weight);
      }
    }
    writer.u32(static_cast<std::uint32_t>(snapshot.metadata.bindings.size()));
    for (const QueueClassBinding& binding : snapshot.metadata.bindings) {
      writer.name_id(binding.queue.device.value());
      writer.name_id(binding.queue.port.value());
      writer.u32(binding.queue.queue.value());
      writer.u8(binding.traffic_class.has_value() ? 1u : 0u);
      if (binding.traffic_class.has_value()) {
        writer.u16(binding.traffic_class->value());
      }
      writer.u8(binding.scheduling_class.has_value() ? 1u : 0u);
      if (binding.scheduling_class.has_value()) {
        writer.name_id(binding.scheduling_class->value());
      }
    }
    if (!writer.ok()) {
      return Status::failure(ErrorCode::LimitExceeded,
                             "the metadata table exceeds the configured record size");
    }
    QOBS_TRY(append(RecordType::MetadataTable, value));
  }

  std::vector<std::byte> header;
  if (!write_header(header, snapshot.policy_version,
                    static_cast<std::uint32_t>(snapshot.runtime_id.size()),
                    snapshot.created.wall.ns, snapshot.created.steady.ns, record_count,
                    static_cast<std::uint64_t>(payload.size()), crc32c(payload))) {
    return Status::failure(ErrorCode::Internal,
                           "the persistence segment header could not be encoded");
  }
  out.clear();
  out.reserve(header.size() + snapshot.runtime_id.size() + payload.size());
  out.insert(out.end(), header.begin(), header.end());
  const auto* runtime_bytes = reinterpret_cast<const std::byte*>(snapshot.runtime_id.data());
  out.insert(out.end(), runtime_bytes, runtime_bytes + snapshot.runtime_id.size());
  out.insert(out.end(), payload.begin(), payload.end());
  return Status::success();
}

namespace {

bool read_count(ByteReader& reader, std::uint32_t& count) {
  if (!reader.u32(count)) {
    return false;
  }
  // Every element occupies at least one byte, so a count larger than the
  // remaining buffer is corrupt by construction. Refusing it up front keeps a
  // damaged file from driving a large allocation.
  if (static_cast<std::uint64_t>(count) > reader.remaining()) {
    return false;
  }
  return true;
}

bool decode_queue_record(ByteReader& reader, const PersistenceLimits& limits,
                         DurableQueueState& queue) {
  std::string device;
  std::string port;
  std::uint32_t queue_id = 0;
  if (!reader.name_id(device) || !reader.name_id(port) || !reader.u32(queue_id)) {
    return false;
  }
  const auto parsed_device = DeviceId::create(device);
  const auto parsed_port = PortId::create(port);
  if (!parsed_device.has_value() || !parsed_port.has_value()) {
    return false;
  }
  queue.queue.device = parsed_device.value();
  queue.queue.port = parsed_port.value();
  queue.queue.queue = QueueId::from_raw(queue_id);

  std::uint8_t classes_known = 0;
  std::uint8_t has_tc = 0;
  std::uint16_t tc = 0;
  std::uint8_t has_sc = 0;
  std::string sc;
  std::uint8_t origin = 0;
  std::uint64_t metadata_revision = 0;
  if (!reader.u8(classes_known) || !reader.u8(has_tc)) {
    return false;
  }
  if (has_tc != 0u && !reader.u16(tc)) {
    return false;
  }
  if (!reader.u8(has_sc)) {
    return false;
  }
  if (has_sc != 0u && !reader.name_id(sc)) {
    return false;
  }
  if (!reader.u8(origin) || !reader.u64(metadata_revision)) {
    return false;
  }
  queue.classes_known = classes_known != 0u;
  if (has_tc != 0u) {
    queue.classes.traffic_class = TrafficClassId::from_raw(tc);
  }
  if (has_sc != 0u) {
    const auto parsed = SchedulingClassId::create(sc);
    if (!parsed.has_value()) {
      return false;
    }
    queue.classes.scheduling_class = parsed.value();
  }
  queue.classes.origin = static_cast<AttributeOrigin>(origin);
  queue.classes.metadata_revision = Revision::from_raw(metadata_revision);

  std::uint32_t sample_count = 0;
  if (!read_count(reader, sample_count)) {
    return false;
  }
  if (static_cast<std::uint64_t>(sample_count) > limits.max_segment_records) {
    return false;
  }
  queue.samples.reserve(sample_count);
  for (std::uint32_t index = 0; index < sample_count; ++index) {
    DurableSample sample;
    std::string domain;
    std::string source;
    if (!reader.i64(sample.observed.ns) || !reader.name_id(domain)) {
      return false;
    }
    if (!domain.empty()) {
      const auto parsed = ClockDomainId::create(domain);
      if (!parsed.has_value()) {
        return false;
      }
      sample.observed.domain = parsed.value();
    }
    if (!reader.i64(sample.received_wall.ns) || !reader.i64(sample.received_steady.ns)) {
      return false;
    }
    if (!reader.name_id(source)) {
      return false;
    }
    if (!source.empty()) {
      const auto parsed = SourceId::create(source);
      if (!parsed.has_value()) {
        return false;
      }
      sample.source = parsed.value();
    }
    std::string incarnation;
    if (!reader.name_id(incarnation)) {
      return false;
    }
    if (!incarnation.empty()) {
      const auto parsed = IncarnationId::create(incarnation);
      if (!parsed.has_value()) {
        return false;
      }
      sample.incarnation = parsed.value();
    }
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;
    std::uint8_t authority = 0;
    if (!reader.u64(generation) || !reader.u64(sequence) || !reader.u8(authority)) {
      return false;
    }
    sample.generation = GenerationId::from_raw(generation);
    sample.sequence = SourceSequence::from_raw(sequence);
    sample.authority = static_cast<SourceAuthority>(authority);
    if (!reader.u64(sample.reported) || !reader.sample_bitmap(sample.reported)) {
      return false;
    }
    for (std::size_t field_index = 0; field_index < kSampleFieldCount; ++field_index) {
      const auto field = static_cast<SampleField>(field_index);
      if (has_field(sample.reported, field) && !reader.u64(sample.values[field_index])) {
        return false;
      }
    }
    std::uint8_t state = 0;
    std::uint8_t freshness = 0;
    std::uint8_t quality = 0;
    if (!reader.u8(state) || !reader.u8(freshness) || !reader.u8(quality) ||
        !reader.i64(sample.age_ns)) {
      return false;
    }
    if (state >= kPressureStateCount) {
      return false;
    }
    sample.state = static_cast<PressureState>(state);
    sample.freshness = static_cast<Freshness>(freshness);
    sample.quality = static_cast<EvidenceQuality>(quality);
    queue.samples.push_back(std::move(sample));
  }
  return true;
}

bool decode_source_registry(ByteReader& reader, DurableSnapshot& out) {
  std::uint32_t count = 0;
  if (!read_count(reader, count)) {
    return false;
  }
  out.sources.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    SourceRecord record;
    std::string id;
    std::string incarnation;
    std::string domain;
    std::uint8_t authority = 0;
    std::uint8_t declared = 0;
    if (!reader.name_id(id)) {
      return false;
    }
    const auto parsed_id = SourceId::create(id);
    if (!parsed_id.has_value()) {
      return false;
    }
    record.id = parsed_id.value();
    if (!reader.u8(authority) || !reader.u8(declared)) {
      return false;
    }
    record.authority = static_cast<SourceAuthority>(authority);
    record.declared_authority = static_cast<SourceAuthority>(declared);
    if (!reader.name_id(incarnation)) {
      return false;
    }
    if (!incarnation.empty()) {
      const auto parsed = IncarnationId::create(incarnation);
      if (!parsed.has_value()) {
        return false;
      }
      record.incarnation = parsed.value();
    }
    if (!reader.u64(record.incarnation_ordinal)) {
      return false;
    }
    std::uint64_t sequence = 0;
    std::uint64_t generation = 0;
    if (!reader.u64(sequence) || !reader.u64(generation)) {
      return false;
    }
    record.last_sequence = SourceSequence::from_raw(sequence);
    record.generation = GenerationId::from_raw(generation);
    if (!reader.name_id(domain)) {
      return false;
    }
    if (!domain.empty()) {
      const auto parsed = ClockDomainId::create(domain);
      if (!parsed.has_value()) {
        return false;
      }
      record.clock_domain = parsed.value();
    }
    if (!reader.i64(record.first_seen.wall.ns) || !reader.i64(record.first_seen.steady.ns) ||
        !reader.i64(record.last_seen.wall.ns) || !reader.i64(record.last_seen.steady.ns)) {
      return false;
    }
    if (!reader.u64(record.accepted) || !reader.u64(record.fenced) ||
        !reader.u64(record.duplicates) || !reader.u64(record.rejected)) {
      return false;
    }
    std::int64_t last_observed = 0;
    if (!reader.i64(last_observed)) {
      return false;
    }
    record.last_observed_ns = static_cast<std::uint64_t>(last_observed);
    std::uint32_t incarnation_count = 0;
    if (!read_count(reader, incarnation_count)) {
      return false;
    }
    for (std::uint32_t entry = 0; entry < incarnation_count; ++entry) {
      std::string token;
      std::uint64_t ordinal = 0;
      if (!reader.name_id(token) || !reader.u64(ordinal)) {
        return false;
      }
      const auto parsed = IncarnationId::create(token);
      if (!parsed.has_value()) {
        return false;
      }
      record.recent_incarnations.emplace_back(parsed.value(), ordinal);
    }
    out.sources.push_back(std::move(record));
  }
  return true;
}

bool decode_event_batch(ByteReader& reader, DurableSnapshot& out) {
  std::uint32_t count = 0;
  if (!read_count(reader, count)) {
    return false;
  }
  out.events.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    EventRecord record;
    std::uint8_t kind = 0;
    std::uint8_t severity = 0;
    if (!reader.u8(kind) || !reader.u8(severity)) {
      return false;
    }
    if (kind >= kEventKindCount) {
      return false;
    }
    record.kind = static_cast<EventKind>(kind);
    record.severity = static_cast<Severity>(severity);
    if (!reader.i64(record.received.wall.ns) || !reader.i64(record.received.steady.ns) ||
        !reader.i64(record.observed.ns)) {
      return false;
    }
    std::string domain;
    std::string device;
    std::string port;
    std::string source;
    std::string incarnation;
    if (!reader.name_id(domain) || !reader.name_id(device) || !reader.name_id(port)) {
      return false;
    }
    std::uint32_t queue_id = 0;
    if (!reader.u32(queue_id)) {
      return false;
    }
    record.queue.queue = QueueId::from_raw(queue_id);
    if (!reader.name_id(source) || !reader.name_id(incarnation)) {
      return false;
    }
    std::uint64_t sequence = 0;
    std::uint64_t revision = 0;
    if (!reader.u64(sequence) || !reader.u64(revision) || !reader.string(record.detail, 512u)) {
      return false;
    }
    if (!domain.empty()) {
      const auto parsed = ClockDomainId::create(domain);
      if (!parsed.has_value()) {
        return false;
      }
      record.observed.domain = parsed.value();
    }
    if (!device.empty()) {
      const auto parsed = DeviceId::create(device);
      if (!parsed.has_value()) {
        return false;
      }
      record.queue.device = parsed.value();
    }
    if (!port.empty()) {
      const auto parsed = PortId::create(port);
      if (!parsed.has_value()) {
        return false;
      }
      record.queue.port = parsed.value();
    }
    if (!source.empty()) {
      const auto parsed = SourceId::create(source);
      if (!parsed.has_value()) {
        return false;
      }
      record.source = parsed.value();
    }
    if (!incarnation.empty()) {
      const auto parsed = IncarnationId::create(incarnation);
      if (!parsed.has_value()) {
        return false;
      }
      record.incarnation = parsed.value();
    }
    record.sequence = SourceSequence::from_raw(sequence);
    record.revision = Revision::from_raw(revision);
    out.events.push_back(std::move(record));
  }
  return true;
}

bool decode_metadata_table(ByteReader& reader, DurableSnapshot& out) {
  std::uint64_t revision = 0;
  std::uint64_t generation = 0;
  std::string source;
  std::uint8_t authority = 0;
  if (!reader.u64(revision) || !reader.u64(generation) || !reader.name_id(source) ||
      !reader.u8(authority)) {
    return false;
  }
  out.metadata.revision = Revision::from_raw(revision);
  out.metadata.generation = GenerationId::from_raw(generation);
  const auto parsed_source = SourceId::create(source);
  if (!parsed_source.has_value()) {
    return false;
  }
  out.metadata.source = parsed_source.value();
  out.metadata.authority = static_cast<SourceAuthority>(authority);
  if (!reader.i64(out.metadata.received.wall.ns) || !reader.i64(out.metadata.received.steady.ns)) {
    return false;
  }

  std::uint32_t traffic_count = 0;
  if (!read_count(reader, traffic_count)) {
    return false;
  }
  out.metadata.traffic_classes.reserve(traffic_count);
  for (std::uint32_t index = 0; index < traffic_count; ++index) {
    TrafficClassDescriptor descriptor;
    std::uint16_t id = 0;
    if (!reader.u16(id) || !reader.string(descriptor.name, 128u)) {
      return false;
    }
    descriptor.id = TrafficClassId::from_raw(id);
    std::uint8_t present = 0;
    if (!reader.u8(present)) {
      return false;
    }
    if (present != 0u) {
      std::uint16_t dscp = 0;
      if (!reader.u16(dscp)) {
        return false;
      }
      descriptor.dscp = dscp;
    }
    if (!reader.u8(present)) {
      return false;
    }
    if (present != 0u) {
      std::uint16_t pcp = 0;
      if (!reader.u16(pcp)) {
        return false;
      }
      descriptor.priority_code_point = pcp;
    }
    if (!reader.u8(present)) {
      return false;
    }
    if (present != 0u) {
      std::string sc;
      if (!reader.name_id(sc)) {
        return false;
      }
      const auto parsed = SchedulingClassId::create(sc);
      if (!parsed.has_value()) {
        return false;
      }
      descriptor.scheduling_class = parsed.value();
    }
    out.metadata.traffic_classes.push_back(std::move(descriptor));
  }

  std::uint32_t scheduling_count = 0;
  if (!read_count(reader, scheduling_count)) {
    return false;
  }
  out.metadata.scheduling_classes.reserve(scheduling_count);
  for (std::uint32_t index = 0; index < scheduling_count; ++index) {
    SchedulingClassDescriptor descriptor;
    std::string id;
    if (!reader.name_id(id) || !reader.string(descriptor.mode, 64u)) {
      return false;
    }
    const auto parsed = SchedulingClassId::create(id);
    if (!parsed.has_value()) {
      return false;
    }
    descriptor.id = parsed.value();
    std::uint8_t present = 0;
    if (!reader.u8(present)) {
      return false;
    }
    if (present != 0u) {
      std::uint64_t weight = 0;
      if (!reader.u64(weight)) {
        return false;
      }
      descriptor.weight = weight;
    }
    out.metadata.scheduling_classes.push_back(std::move(descriptor));
  }

  std::uint32_t binding_count = 0;
  if (!read_count(reader, binding_count)) {
    return false;
  }
  out.metadata.bindings.reserve(binding_count);
  for (std::uint32_t index = 0; index < binding_count; ++index) {
    QueueClassBinding binding;
    std::string device;
    std::string port;
    std::uint32_t queue_id = 0;
    if (!reader.name_id(device) || !reader.name_id(port) || !reader.u32(queue_id)) {
      return false;
    }
    const auto parsed_device = DeviceId::create(device);
    const auto parsed_port = PortId::create(port);
    if (!parsed_device.has_value() || !parsed_port.has_value()) {
      return false;
    }
    binding.queue.device = parsed_device.value();
    binding.queue.port = parsed_port.value();
    binding.queue.queue = QueueId::from_raw(queue_id);
    std::uint8_t present = 0;
    if (!reader.u8(present)) {
      return false;
    }
    if (present != 0u) {
      std::uint16_t tc = 0;
      if (!reader.u16(tc)) {
        return false;
      }
      binding.traffic_class = TrafficClassId::from_raw(tc);
    }
    if (!reader.u8(present)) {
      return false;
    }
    if (present != 0u) {
      std::string sc;
      if (!reader.name_id(sc)) {
        return false;
      }
      const auto parsed = SchedulingClassId::create(sc);
      if (!parsed.has_value()) {
        return false;
      }
      binding.scheduling_class = parsed.value();
    }
    out.metadata.bindings.push_back(std::move(binding));
  }
  out.has_metadata = true;
  return true;
}

}  // namespace

Status decode_snapshot(std::span<const std::byte> bytes, const PersistenceLimits& limits,
                       DurableSnapshot& out, DecodeReport& report) {
  out = DurableSnapshot{};
  report = DecodeReport{};
  const Status limits_status = validate_limits(limits);
  if (!limits_status.ok()) {
    return limits_status;
  }
  if (bytes.size() < kSegmentHeaderSize) {
    report.diagnostic = "the segment is smaller than its fixed header";
    return Status::failure(ErrorCode::IntegrityFailure, report.diagnostic);
  }
  if (std::memcmp(bytes.data(), kSegmentMagic, sizeof(kSegmentMagic)) != 0) {
    report.diagnostic = "the segment magic does not match";
    return Status::failure(ErrorCode::IntegrityFailure, report.diagnostic);
  }
  report.magic_valid = true;

  ByteReader reader(bytes.subspan(sizeof(kSegmentMagic)), limits.max_record_bytes);
  std::uint32_t format_version = 0;
  std::uint32_t header_size = 0;
  std::uint32_t header_crc = 0;
  std::uint32_t policy_version = 0;
  std::uint32_t runtime_id_len = 0;
  std::uint32_t flags = 0;
  std::int64_t created_wall = 0;
  std::int64_t created_steady = 0;
  std::uint64_t record_count = 0;
  std::uint64_t payload_bytes = 0;
  std::uint32_t payload_crc = 0;
  std::uint32_t runtime_major = 0;
  std::uint32_t runtime_minor = 0;
  std::uint32_t runtime_patch = 0;
  const bool header_ok =
      reader.u32(format_version) && reader.u32(header_size) && reader.u32(header_crc) &&
      reader.u32(policy_version) && reader.u32(runtime_id_len) && reader.u32(flags) &&
      reader.i64(created_wall) && reader.i64(created_steady) && reader.u64(record_count) &&
      reader.u64(payload_bytes) && reader.u32(payload_crc) && reader.u32(runtime_major) &&
      reader.u32(runtime_minor) && reader.u32(runtime_patch);
  if (!header_ok) {
    report.diagnostic = "the segment header is truncated";
    return Status::failure(ErrorCode::IntegrityFailure, report.diagnostic);
  }
  report.format_version = format_version;
  report.created_wall = WallTime{created_wall};
  report.records_declared = record_count;
  report.bytes_declared = payload_bytes;

  if (header_size != kSegmentHeaderSize) {
    report.diagnostic = "the segment header declares an unexpected size";
    return Status::failure(ErrorCode::VersionMismatch, report.diagnostic);
  }
  {
    std::vector<std::byte> header_copy(bytes.begin(),
                                       bytes.begin() + static_cast<std::ptrdiff_t>(kSegmentHeaderSize));
    header_copy[16] = std::byte{0};
    header_copy[17] = std::byte{0};
    header_copy[18] = std::byte{0};
    header_copy[19] = std::byte{0};
    if (crc32c(header_copy) != header_crc) {
      report.diagnostic = "the segment header checksum does not match its contents";
      return Status::failure(ErrorCode::IntegrityFailure, report.diagnostic);
    }
  }
  report.header_crc_valid = true;

  if (format_version != QOBS_PERSISTENCE_FORMAT_VERSION) {
    report.diagnostic = "the segment was written in an unsupported format version";
    return Status::failure(ErrorCode::VersionMismatch, report.diagnostic);
  }
  report.version_supported = true;

  if (runtime_id_len > kMaxRuntimeIdBytes) {
    report.diagnostic = "the segment declares an oversized runtime identifier";
    return Status::failure(ErrorCode::IntegrityFailure, report.diagnostic);
  }
  const std::uint64_t body_start =
      static_cast<std::uint64_t>(kSegmentHeaderSize) + runtime_id_len;
  if (body_start > static_cast<std::uint64_t>(bytes.size())) {
    report.truncated = true;
    report.integrity_failed = true;
    report.diagnostic = "the segment ends before its runtime identifier is complete";
    return Status::success();
  }
  out.runtime_id.assign(reinterpret_cast<const char*>(bytes.data() + kSegmentHeaderSize),
                        runtime_id_len);
  out.created = ReceiveTime{SteadyTime{created_steady}, WallTime{created_wall}};
  out.policy_version = policy_version;

  const std::uint64_t available = static_cast<std::uint64_t>(bytes.size()) - body_start;
  std::uint64_t usable = payload_bytes;
  if (usable > available) {
    usable = available;
    report.truncated = true;
    report.integrity_failed = true;
  }
  const std::span<const std::byte> payload(bytes.data() + body_start,
                                           static_cast<std::size_t>(usable));
  const std::uint32_t computed_payload_crc = crc32c(payload);
  if (computed_payload_crc != payload_crc) {
    report.payload_crc_valid = false;
    report.integrity_failed = true;
    if (report.diagnostic.empty()) {
      report.diagnostic = "the payload checksum does not match the stored value: stored=" +
                          text::to_hex(payload_crc, 8u) + " computed=" +
                          text::to_hex(computed_payload_crc, 8u) + " payload_bytes=" +
                          std::to_string(usable) + " segment_bytes=" +
                          std::to_string(bytes.size()) + " body_offset=" +
                          std::to_string(body_start);
    }
  } else {
    report.payload_crc_valid = true;
  }

  std::uint64_t offset = 0;
  while (offset + kRecordHeaderSize <= usable) {
    ByteReader record_reader(payload.subspan(static_cast<std::size_t>(offset)),
                             limits.max_record_bytes);
    std::uint32_t stored_crc = 0;
    std::uint8_t type = 0;
    std::uint8_t record_flags = 0;
    std::uint16_t reserved = 0;
    std::uint32_t value_len = 0;
    if (!record_reader.u32(stored_crc) || !record_reader.u8(type) ||
        !record_reader.u8(record_flags) || !record_reader.u16(reserved) ||
        !record_reader.u32(value_len)) {
      report.truncated = true;
      report.integrity_failed = true;
      report.diagnostic = "a record header is truncated";
      break;
    }
    if (value_len > limits.max_record_bytes) {
      report.integrity_failed = true;
      report.diagnostic = "a record declares a value larger than the configured limit";
      break;
    }
    const std::uint64_t record_total =
        static_cast<std::uint64_t>(kRecordHeaderSize) + value_len;
    if (offset + record_total > usable) {
      report.truncated = true;
      report.integrity_failed = true;
      report.diagnostic = "the final record is cut short by the end of the segment";
      break;
    }
    const std::byte* value_begin = payload.data() + offset + kRecordHeaderSize;
    const std::array<std::byte, 8> prefix =
        record_prefix(static_cast<RecordType>(type), record_flags, value_len);
    Crc32c crc;
    crc.update(std::span<const std::byte>(prefix.data(), prefix.size()));
    crc.update(std::span<const std::byte>(value_begin, value_len));
    if (crc.value() != stored_crc) {
      report.integrity_failed = true;
      report.records_rejected += 1u;
      report.diagnostic = "record " + std::to_string(report.records_parsed) +
                          " checksum mismatch: stored=" + text::to_hex(stored_crc, 8u) +
                          " computed=" + text::to_hex(crc.value(), 8u) + " type=" +
                          std::to_string(type) + " value_bytes=" + std::to_string(value_len);
      break;
    }

    ByteReader value_reader(std::span<const std::byte>(value_begin, value_len),
                            limits.max_record_bytes);
    bool parsed = true;
    switch (static_cast<RecordType>(type)) {
      case RecordType::SessionHeader: {
        std::string runtime_id;
        std::uint32_t session_policy = 0;
        std::string policy_name;
        std::int64_t wall = 0;
        std::int64_t steady = 0;
        std::uint32_t declared_queues = 0;
        parsed = value_reader.string(runtime_id, static_cast<std::uint32_t>(kMaxRuntimeIdBytes)) &&
                 value_reader.u32(session_policy) &&
                 value_reader.string(policy_name, 128u) && value_reader.i64(wall) &&
                 value_reader.i64(steady) && value_reader.u32(declared_queues);
        if (parsed) {
          if (!runtime_id.empty()) {
            out.runtime_id = runtime_id;
          }
          out.policy_version = session_policy;
          out.policy_name = policy_name;
        }
        break;
      }
      case RecordType::QueueSnapshot: {
        DurableQueueState queue;
        parsed = decode_queue_record(value_reader, limits, queue);
        if (parsed) {
          out.queues.push_back(std::move(queue));
        }
        break;
      }
      case RecordType::EventBatch:
        parsed = decode_event_batch(value_reader, out);
        break;
      case RecordType::SourceRegistry:
        parsed = decode_source_registry(value_reader, out);
        break;
      case RecordType::MetadataTable:
        parsed = decode_metadata_table(value_reader, out);
        break;
      default:
        parsed = false;
        break;
    }
    if (!parsed) {
      report.integrity_failed = true;
      report.records_rejected += 1u;
      if (report.diagnostic.empty()) {
        report.diagnostic = "a record could not be decoded";
      }
      break;
    }
    report.records_parsed += 1u;
    offset += record_total;
  }
  report.bytes_consumed = offset;
  if (offset != usable) {
    report.integrity_failed = true;
    if (report.diagnostic.empty()) {
      report.diagnostic = "the segment contains trailing bytes that are not part of a record";
    }
  }
  if (report.records_parsed < record_count) {
    report.integrity_failed = true;
    if (report.diagnostic.empty()) {
      report.diagnostic = "fewer records were readable than the header declares";
    }
  }
  return Status::success();
}

Status build_snapshot(const QueueStore& store, std::size_t max_samples_per_queue,
                      std::size_t max_events, DurableSnapshot& out) {
  out = DurableSnapshot{};
  const SessionInfo session = store.session();
  out.runtime_id = session.runtime_id;
  out.created = session.started;
  out.policy_version = session.policy_version;
  out.policy_name = session.policy_name;

  InspectQuery inspect_query;
  inspect_query.page.limit = 0u;
  InspectResult inspected;
  QOBS_TRY(store.inspect(inspect_query, inspected));

  // History records carry the incarnation ordinal rather than the opaque token,
  // because a token per retained record would be an unbounded cost. The source
  // registry holds the mapping, so it is read first and used to restore the
  // token each record was produced under.
  QOBS_TRY(store.sources(out.sources));
  std::map<std::pair<SourceId, IncarnationOrdinal>, IncarnationId> incarnation_tokens;
  for (const SourceRecord& source : out.sources) {
    if (source.incarnation.valid()) {
      incarnation_tokens[{source.id, source.incarnation_ordinal}] = source.incarnation;
    }
    for (const auto& entry : source.recent_incarnations) {
      incarnation_tokens[{source.id, entry.second}] = entry.first;
    }
  }

  const Status walk = store.for_each_queue(
      [&out, &incarnation_tokens, max_samples_per_queue](
          const QueuePath& path, const std::vector<HistoryRecord>& records) {
        DurableQueueState queue;
        queue.queue = path;
        const std::size_t take =
            records.size() > max_samples_per_queue ? max_samples_per_queue : records.size();
        const std::size_t start = records.size() - take;
        queue.samples.reserve(take);
        for (std::size_t index = start; index < records.size(); ++index) {
          const HistoryRecord& record = records[index];
          DurableSample sample;
          sample.observed = record.observed;
          sample.received_wall = record.received.wall;
          sample.received_steady = record.received.steady;
          sample.source = record.source;
          const auto token = incarnation_tokens.find({record.source, record.incarnation_ordinal});
          if (token != incarnation_tokens.end()) {
            sample.incarnation = token->second;
          }
          sample.generation = record.generation;
          sample.sequence = record.sequence;
          sample.authority = record.authority;
          sample.reported = record.reported;
          for (std::size_t field = 0; field < kSampleFieldCount; ++field) {
            sample.values[field] = record.values[field];
          }
          sample.state = record.state;
          sample.freshness = record.freshness;
          sample.quality = record.quality;
          sample.age_ns = record.age_ns;
          queue.samples.push_back(std::move(sample));
        }
        out.queues.push_back(std::move(queue));
      },
      static_cast<std::size_t>(1) << 20);
  QOBS_TRY(walk);

  for (const InspectRow& row : inspected.rows) {
    for (DurableQueueState& queue : out.queues) {
      if (queue.queue == row.queue) {
        queue.classes = row.classes;
        queue.classes_known = row.classes.origin != AttributeOrigin::Unspecified;
        break;
      }
    }
  }

  EventQuery event_query;
  event_query.page.limit = max_events;
  EventResult event_result;
  QOBS_TRY(store.events(event_query, event_result));
  out.events = std::move(event_result.events);

  ClassMetadataTable metadata;
  bool has_metadata = false;
  QOBS_TRY(store.metadata_snapshot(metadata, has_metadata));
  out.metadata = std::move(metadata);
  out.has_metadata = has_metadata;
  return Status::success();
}

}  // namespace qobs
