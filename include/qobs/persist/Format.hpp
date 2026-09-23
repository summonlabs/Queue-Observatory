#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "qobs/core/Limits.hpp"
#include "qobs/core/Result.hpp"
#include "qobs/model/Event.hpp"
#include "qobs/model/Metadata.hpp"
#include "qobs/model/Sample.hpp"
#include "qobs/policy/Policy.hpp"
#include "qobs/store/Store.hpp"

namespace qobs {

/// Magic bytes at the start of every persistence segment.
inline constexpr char kSegmentMagic[8] = {'Q', 'O', 'B', 'S', 'S', 'E', 'G', '1'};

/// Layout of the fixed segment header, in bytes:
///
///   magic                       8
///   format version              4
///   header size                 4
///   header checksum             4
///   policy version              4
///   runtime identifier length   4
///   reserved flags              4
///   created (wall)              8
///   created (steady)            8
///   record count                8
///   payload bytes               8
///   payload checksum            4
///   writer version major        4
///   writer version minor        4
///   writer version patch        4
///
/// The size is written as the sum of its parts so that adding a field without
/// updating the total is a compile-time error rather than a silent truncation.
inline constexpr std::uint32_t kSegmentHeaderSize = 8u + (4u * 6u) + (8u * 4u) + (4u * 4u);
inline constexpr std::uint32_t kRecordHeaderSize = 12u;
inline constexpr std::size_t kMaxRuntimeIdBytes = 64u;

/// One retained observation in durable form.
///
/// The steady receive time is persisted even though it is only meaningful
/// inside the process that recorded it: recovery uses its presence to prove
/// that the value cannot be compared with the new process's steady clock, which
/// is exactly why recovered evidence is never reported as fresh.
struct DurableSample {
  ObservationTime observed{};
  WallTime received_wall{};
  SteadyTime received_steady{};
  SourceId source{};
  /// The incarnation token the sample was produced under. Without it the
  /// evidence cannot be attributed on recovery and must be refused rather than
  /// guessed at.
  IncarnationId incarnation{};
  GenerationId generation{};
  SourceSequence sequence{};
  SourceAuthority authority{SourceAuthority::Unknown};
  FieldMask reported{0};
  std::uint64_t values[kSampleFieldCount]{};
  PressureState state{PressureState::Unknown};
  Freshness freshness{Freshness::Unknown};
  EvidenceQuality quality{EvidenceQuality::Unknown};
  Nanos age_ns{0};
};

struct DurableQueueState {
  QueuePath queue{};
  QueueClassAttributes classes{};
  bool classes_known{false};
  std::vector<DurableSample> samples{};
};

/// Everything a restart needs to reconstruct history without inventing any.
struct DurableSnapshot {
  std::string runtime_id{};
  ReceiveTime created{};
  std::uint32_t policy_version{0};
  std::string policy_name{};
  std::vector<DurableQueueState> queues{};
  std::vector<EventRecord> events{};
  std::vector<SourceRecord> sources{};
  ClassMetadataTable metadata{};
  bool has_metadata{false};
};

/// Facts about what a decoder actually managed to read. Recovery is never
/// silent: every field here is reported to the caller.
struct DecodeReport {
  bool magic_valid{false};
  bool header_crc_valid{false};
  bool payload_crc_valid{false};
  bool version_supported{false};
  bool truncated{false};
  bool integrity_failed{false};
  std::uint64_t records_declared{0};
  std::uint64_t records_parsed{0};
  std::uint64_t records_rejected{0};
  std::uint64_t bytes_consumed{0};
  std::uint64_t bytes_declared{0};
  std::uint32_t format_version{0};
  WallTime created_wall{};
  std::string diagnostic{};

  [[nodiscard]] bool clean() const noexcept {
    return magic_valid && header_crc_valid && payload_crc_valid && version_supported &&
           !truncated && !integrity_failed;
  }
};

/// Encode a snapshot. The output is byte-for-byte deterministic for identical
/// input, so a persisted file can be diffed and re-verified.
[[nodiscard]] Status encode_snapshot(const DurableSnapshot& snapshot,
                                     const PersistenceLimits& limits,
                                     std::vector<std::byte>& out);

/// Decode a snapshot with conservative recovery: decoding stops at the first
/// damaged record and everything read up to that point is returned along with a
/// report that says exactly what happened.
[[nodiscard]] Status decode_snapshot(std::span<const std::byte> bytes,
                                     const PersistenceLimits& limits, DurableSnapshot& out,
                                     DecodeReport& report);

/// Reduce a live store's retained evidence to a durable snapshot. The number of
/// samples retained per queue is bounded by max_samples_per_queue.
[[nodiscard]] Status build_snapshot(const QueueStore& store, std::size_t max_samples_per_queue,
                                    std::size_t max_events, DurableSnapshot& out);

}  // namespace qobs
