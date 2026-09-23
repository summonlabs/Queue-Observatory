#include "qobs/persist/Persistence.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <system_error>

#include "qobs/core/LockAudit.hpp"
#include "qobs/core/Text.hpp"
#include "qobs/version.hpp"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace qobs {
namespace {

constexpr std::string_view kSegmentPrefix = "segment-";
constexpr std::string_view kSegmentSuffix = ".qobs";
constexpr std::string_view kTempSuffix = ".qobs.tmp";

std::string zero_pad(std::uint64_t value, std::size_t width) {
  std::string digits = std::to_string(value);
  while (digits.size() < width) {
    digits.insert(digits.begin(), '0');
  }
  return digits;
}

}  // namespace

std::string segment_file_name(std::uint64_t sequence) {
  return std::string(kSegmentPrefix) + zero_pad(sequence, 8u) + std::string(kSegmentSuffix);
}

std::string segment_temp_name(std::uint64_t sequence) {
  return std::string(kSegmentPrefix) + zero_pad(sequence, 8u) + std::string(kTempSuffix);
}

bool parse_segment_file_name(std::string_view name, std::uint64_t& sequence) {
  if (!text::starts_with(name, kSegmentPrefix) || !text::ends_with(name, kSegmentSuffix)) {
    return false;
  }
  const auto digits = name.substr(kSegmentPrefix.size(),
                                  name.size() - kSegmentPrefix.size() - kSegmentSuffix.size());
  return text::parse_u64(digits, sequence);
}

Status flush_and_sync(std::FILE* stream) {
  if (std::fflush(stream) != 0) {
    return Status::failure(ErrorCode::IoError, "flushing the persistence file failed");
  }
#ifdef _WIN32
  if (_commit(_fileno(stream)) != 0) {
    return Status::failure(ErrorCode::IoError, "syncing the persistence file failed");
  }
#else
  if (::fsync(::fileno(stream)) != 0) {
    return Status::failure(ErrorCode::IoError, "syncing the persistence file failed");
  }
#endif
  return Status::success();
}

Status read_file_bounded(const std::filesystem::path& path, std::uint64_t max_bytes,
                         std::vector<std::byte>& out, std::string& diagnostic) {
  out.clear();
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    diagnostic = "the segment size could not be read";
    return Status::failure(ErrorCode::IoError, diagnostic);
  }
  if (size > max_bytes) {
    diagnostic = "the segment is larger than the configured maximum";
    return Status::failure(ErrorCode::LimitExceeded, diagnostic);
  }
  std::FILE* stream = std::fopen(path.string().c_str(), "rb");
  if (stream == nullptr) {
    diagnostic = "the segment could not be opened for reading";
    return Status::failure(ErrorCode::IoError, diagnostic);
  }
  std::vector<std::byte> buffer(static_cast<std::size_t>(size));
  const std::size_t read = buffer.empty() ? 0u : std::fread(buffer.data(), 1u, buffer.size(), stream);
  const bool short_read = read != buffer.size();
  std::fclose(stream);
  if (short_read) {
    diagnostic = "the segment could not be read completely";
    return Status::failure(ErrorCode::IoError, diagnostic);
  }
  out = std::move(buffer);
  return Status::success();
}

PersistenceStore::PersistenceStore(std::filesystem::path directory, PersistenceLimits limits,
                                   std::shared_ptr<Clock> clock)
    : directory_(std::move(directory)), limits_(limits), clock_(std::move(clock)) {}

PersistenceStore::~PersistenceStore() = default;

Status PersistenceStore::open() {
  std::lock_guard<std::mutex> lock(mutex_);
  LockRankGuard guard(kRankPersistence, "persistence");
  const Status limits_status = validate_limits(limits_);
  if (!limits_status.ok()) {
    return limits_status;
  }
  std::error_code error;
  std::filesystem::create_directories(directory_, error);
  if (error) {
    return Status::failure(ErrorCode::IoError,
                           "the persistence directory could not be created: " +
                               directory_.string());
  }
  if (!std::filesystem::is_directory(directory_, error) || error) {
    return Status::failure(ErrorCode::IoError,
                           "the persistence path is not a directory: " + directory_.string());
  }
  std::vector<SegmentInfo> segments;
  QOBS_TRY(list_segments(segments));
  next_sequence_ = 1;
  for (const SegmentInfo& segment : segments) {
    if (segment.sequence >= next_sequence_) {
      next_sequence_ = segment.sequence + 1u;
    }
  }
  return Status::success();
}

Status PersistenceStore::list_segments(std::vector<SegmentInfo>& segments) const {
  segments.clear();
  std::error_code error;
  if (!std::filesystem::exists(directory_, error) || error) {
    return Status::success();
  }
  for (const auto& entry : std::filesystem::directory_iterator(directory_, error)) {
    if (error) {
      return Status::failure(ErrorCode::IoError, "the persistence directory could not be listed");
    }
    if (!entry.is_regular_file(error) || error) {
      continue;
    }
    std::uint64_t sequence = 0;
    if (!parse_segment_file_name(entry.path().filename().string(), sequence)) {
      continue;
    }
    SegmentInfo info;
    info.path = entry.path();
    info.sequence = sequence;
    info.bytes = static_cast<std::uint64_t>(entry.file_size(error));
    if (error) {
      info.bytes = 0;
      error.clear();
    }
    segments.push_back(std::move(info));
  }
  std::sort(segments.begin(), segments.end(),
            [](const SegmentInfo& left, const SegmentInfo& right) {
              return left.sequence < right.sequence;
            });
  return Status::success();
}

Status PersistenceStore::write_snapshot(const DurableSnapshot& snapshot,
                                        PersistenceReport& report) {
  std::lock_guard<std::mutex> lock(mutex_);
  LockRankGuard guard(kRankPersistence, "persistence");

  std::vector<std::byte> encoded;
  QOBS_TRY(encode_snapshot(snapshot, limits_, encoded));
  if (static_cast<std::uint64_t>(encoded.size()) > limits_.max_segment_bytes) {
    return Status::failure(ErrorCode::LimitExceeded,
                           "the encoded snapshot exceeds the configured segment size");
  }

  const std::uint64_t sequence = next_sequence_;
  const std::filesystem::path temp_path = directory_ / segment_temp_name(sequence);
  const std::filesystem::path final_path = directory_ / segment_file_name(sequence);

  std::FILE* stream = std::fopen(temp_path.string().c_str(), "wb");
  if (stream == nullptr) {
    return Status::failure(ErrorCode::IoError, "the temporary segment could not be created");
  }
  const std::size_t written =
      encoded.empty() ? 0u : std::fwrite(encoded.data(), 1u, encoded.size(), stream);
  if (written != encoded.size()) {
    std::fclose(stream);
    std::error_code cleanup_error;
    std::filesystem::remove(temp_path, cleanup_error);
    return Status::failure(ErrorCode::IoError, "the segment could not be written completely");
  }
  const Status sync_status = flush_and_sync(stream);
  std::fclose(stream);
  if (!sync_status.ok()) {
    std::error_code cleanup_error;
    std::filesystem::remove(temp_path, cleanup_error);
    return sync_status;
  }

  std::error_code error;
  std::filesystem::rename(temp_path, final_path, error);
  if (error) {
    std::filesystem::remove(final_path, error);
    error.clear();
    std::filesystem::rename(temp_path, final_path, error);
    if (error) {
      return Status::failure(ErrorCode::IoError, "the segment could not be published atomically");
    }
  }

  ++next_sequence_;
  stats_.segments_written += 1u;
  stats_.bytes_written += static_cast<std::uint64_t>(encoded.size());
  stats_.records_written += 0u;

  std::vector<SegmentInfo> segments;
  QOBS_TRY(list_segments(segments));
  while (segments.size() > limits_.max_segments) {
    std::error_code remove_error;
    std::filesystem::remove(segments.front().path, remove_error);
    if (remove_error) {
      break;
    }
    segments.erase(segments.begin());
    stats_.segments_rotated += 1u;
  }
  report = stats_;
  return Status::success();
}

Status PersistenceStore::recover(RecoveryOutcome& outcome) {
  std::lock_guard<std::mutex> lock(mutex_);
  LockRankGuard guard(kRankPersistence, "persistence");
  outcome = RecoveryOutcome{};
  stats_.recovery_attempts += 1u;

  std::vector<SegmentInfo> segments;
  QOBS_TRY(list_segments(segments));
  if (segments.empty()) {
    outcome.diagnostic = "no persistence segment is present";
    stats_.recovery_failures += 1u;
    return Status::success();
  }

  std::string first_diagnostic;
  for (auto it = segments.rbegin(); it != segments.rend(); ++it) {
    std::vector<std::byte> bytes;
    std::string diagnostic;
    const Status read_status =
        read_file_bounded(it->path, limits_.max_segment_bytes, bytes, diagnostic);
    if (!read_status.ok()) {
      if (first_diagnostic.empty()) {
        first_diagnostic = diagnostic;
      }
      stats_.damaged_segments_skipped += 1u;
      continue;
    }
    DecodeReport report;
    DurableSnapshot snapshot;
    const Status decode_status = decode_snapshot(bytes, limits_, snapshot, report);
    if (!decode_status.ok()) {
      // A segment whose header cannot be trusted is skipped entirely; there is
      // nothing in it that could be reported as evidence.
      if (first_diagnostic.empty()) {
        first_diagnostic = report.diagnostic.empty() ? decode_status.message()
                                                     : report.diagnostic;
      }
      stats_.damaged_segments_skipped += 1u;
      continue;
    }
    outcome.recovered = true;
    outcome.report = report;
    outcome.snapshot = std::move(snapshot);
    outcome.source = it->path;
    outcome.diagnostic = report.clean()
                             ? "the newest segment decoded cleanly"
                             : "the newest readable segment decoded with damage: " +
                                   report.diagnostic;
    stats_.recovery_successes += 1u;
    return Status::success();
  }

  outcome.diagnostic = first_diagnostic.empty()
                           ? "no persistence segment could be decoded"
                           : first_diagnostic;
  stats_.recovery_failures += 1u;
  return Status::success();
}

Status PersistenceStore::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  LockRankGuard guard(kRankPersistence, "persistence");
  std::error_code error;
  std::vector<std::filesystem::path> doomed;
  for (const auto& entry : std::filesystem::directory_iterator(directory_, error)) {
    if (error) {
      return Status::failure(ErrorCode::IoError, "the persistence directory could not be listed");
    }
    if (!entry.is_regular_file(error) || error) {
      continue;
    }
    const std::string name = entry.path().filename().string();
    std::uint64_t sequence = 0;
    if (parse_segment_file_name(name, sequence) || text::ends_with(name, kTempSuffix)) {
      doomed.push_back(entry.path());
    }
  }
  for (const auto& path : doomed) {
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
    if (remove_error) {
      return Status::failure(ErrorCode::IoError, "a persistence segment could not be removed");
    }
  }
  return Status::success();
}

PersistenceReport PersistenceStore::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

Status PersistenceStore::rotate_locked() { return Status::success(); }

}  // namespace qobs
