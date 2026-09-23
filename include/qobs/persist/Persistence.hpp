#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "qobs/core/Limits.hpp"
#include "qobs/core/Result.hpp"
#include "qobs/core/Time.hpp"
#include "qobs/persist/Format.hpp"

namespace qobs {

struct SegmentInfo {
  std::filesystem::path path{};
  std::uint64_t bytes{0};
  WallTime modified_wall{};
  std::uint64_t sequence{0};
};

struct PersistenceReport {
  std::uint64_t segments_written{0};
  std::uint64_t segments_rotated{0};
  std::uint64_t bytes_written{0};
  std::uint64_t records_written{0};
  std::uint64_t recovery_attempts{0};
  std::uint64_t recovery_successes{0};
  std::uint64_t recovery_failures{0};
  std::uint64_t damaged_segments_skipped{0};
};

struct RecoveryOutcome {
  bool recovered{false};
  DecodeReport report{};
  DurableSnapshot snapshot{};
  std::filesystem::path source{};
  std::string diagnostic{};
};

/// Integrity-checked, bounded, rotation-based persistence.
///
/// Writes are atomic: a snapshot is written to a temporary file, flushed and
/// synced, then renamed over the segment name. A crash therefore leaves either
/// the previous segment or the new one, never a half-written file that a reader
/// could mistake for complete.
///
/// Recovery is conservative. A damaged segment yields the records that were
/// fully readable, a report describing the damage, and evidence that is marked
/// recovered and stale. It never yields evidence that quietly looks current.
class PersistenceStore {
 public:
  PersistenceStore(std::filesystem::path directory, PersistenceLimits limits,
                   std::shared_ptr<Clock> clock);
  ~PersistenceStore();

  PersistenceStore(const PersistenceStore&) = delete;
  PersistenceStore& operator=(const PersistenceStore&) = delete;
  PersistenceStore(PersistenceStore&&) = delete;
  PersistenceStore& operator=(PersistenceStore&&) = delete;

  /// Create the directory if needed and validate the limits.
  [[nodiscard]] Status open();

  /// Persist one snapshot, rotating older segments. Returns the report of what
  /// the filesystem actually received.
  [[nodiscard]] Status write_snapshot(const DurableSnapshot& snapshot, PersistenceReport& report);

  /// Read the newest segment that decodes with a valid header. Falls back to
  /// older segments when the newest one is damaged, and reports which one was
  /// used and why.
  [[nodiscard]] Status recover(RecoveryOutcome& outcome);

  [[nodiscard]] Status list_segments(std::vector<SegmentInfo>& segments) const;

  /// Remove every segment and temporary file in the directory.
  [[nodiscard]] Status clear();

  [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }
  [[nodiscard]] const PersistenceLimits& limits() const noexcept { return limits_; }
  [[nodiscard]] PersistenceReport stats() const;

 private:
  [[nodiscard]] Status rotate_locked();

  std::filesystem::path directory_{};
  PersistenceLimits limits_{};
  std::shared_ptr<Clock> clock_{};
  mutable std::mutex mutex_{};
  std::uint64_t next_sequence_{0};
  PersistenceReport stats_{};
};

/// Name of the segment file for a sequence number.
[[nodiscard]] std::string segment_file_name(std::uint64_t sequence);
/// Name of the temporary file used while writing a segment.
[[nodiscard]] std::string segment_temp_name(std::uint64_t sequence);
/// Parse a segment file name back to its sequence number.
[[nodiscard]] bool parse_segment_file_name(std::string_view name, std::uint64_t& sequence);

/// Flush and sync a stream to stable storage where the platform supports it.
[[nodiscard]] Status flush_and_sync(std::FILE* stream);

/// Read a whole file, refusing anything larger than the supplied bound.
[[nodiscard]] Status read_file_bounded(const std::filesystem::path& path, std::uint64_t max_bytes,
                                       std::vector<std::byte>& out, std::string& diagnostic);

}  // namespace qobs
