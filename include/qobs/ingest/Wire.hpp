#pragma once

#include <cstddef>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "qobs/core/Limits.hpp"
#include "qobs/core/Result.hpp"
#include "qobs/model/Metadata.hpp"
#include "qobs/model/Sample.hpp"
#include "qobs/version.hpp"

namespace qobs {

/// The canonical Queue Observatory observation format.
///
/// It is newline-delimited JSON: one object per line, each carrying a "kind"
/// field. Blank lines and lines beginning with '#' are ignored so that a file
/// can carry comments. The format is deliberately vendor neutral: it describes
/// what was observed, not how any particular device is polled.
namespace wire {

/// Measurement keys accepted inside a queue_sample record, paired with the
/// sample field they fill.
struct MeasurementKey {
  std::string_view key;
  SampleField field;
};

[[nodiscard]] const std::vector<MeasurementKey>& measurement_keys();

/// Version of the canonical format this build writes.
inline constexpr std::uint32_t kVersion = QOBS_WIRE_FORMAT_VERSION;

/// Maximum bytes of one newline-delimited record accepted by the decoder.
inline constexpr std::size_t kMaxLineBytes = 1u << 20;

}  // namespace wire

enum class DecodeStatus : std::uint8_t {
  Ok = 0,
  Skipped,
  Malformed,
  Unsupported,
  VersionMismatch,
};

[[nodiscard]] std::string_view to_string(DecodeStatus status) noexcept;

struct DecodeDiagnostic {
  std::size_t line{0};
  DecodeStatus status{DecodeStatus::Malformed};
  std::string reason{};
};

struct DecodeOutcome {
  std::size_t lines{0};
  std::size_t records{0};
  /// Number of sample records decoded. Distinct from the samples vector, which
  /// holds the decoded values themselves.
  std::size_t sample_count{0};
  std::size_t metadata_count{0};
  std::size_t metadata_records{0};
  std::size_t skipped{0};
  std::size_t malformed{0};
  std::size_t unsupported{0};
  std::size_t version_mismatches{0};
  std::vector<QueueSample> samples{};
  std::vector<ClassMetadataTable> metadata{};
  std::vector<DecodeDiagnostic> diagnostics{};
  bool diagnostics_truncated{false};
  std::size_t unknown_keys{0};
};

/// Decode a newline-delimited document. A malformed line never aborts the
/// document: it is counted, described and skipped, so that one bad record
/// cannot hide the rest.
[[nodiscard]] Status decode_document(std::string_view payload, const IngestLimits& limits,
                                     DecodeOutcome& outcome);

/// Check that a sample carries everything the canonical format requires.
///
/// The encoder refuses a sample that the decoder would refuse, so a producer
/// can never emit a record that its own reader would call malformed.
[[nodiscard]] Status validate_encodable_sample(const QueueSample& sample);

/// Encode one sample as a canonical record. Deterministic and round-trippable.
[[nodiscard]] Status encode_sample(const QueueSample& sample, std::string& out);

/// Encode a class metadata table as a canonical record.
[[nodiscard]] Status encode_metadata(const ClassMetadataTable& table, std::string& out);

}  // namespace qobs
