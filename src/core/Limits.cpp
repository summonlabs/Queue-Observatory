#include "qobs/core/Limits.hpp"

namespace qobs {
namespace {

Status require_positive(std::uint64_t value, const char* name) {
  if (value == 0) {
    return Status::failure(ErrorCode::InvalidArgument, std::string(name) + " must be non-zero");
  }
  return Status::success();
}

Status require_positive_size(std::size_t value, const char* name) {
  if (value == 0) {
    return Status::failure(ErrorCode::InvalidArgument, std::string(name) + " must be non-zero");
  }
  return Status::success();
}

Status require_le(std::size_t value, std::size_t limit, const char* name, const char* limit_name) {
  if (value > limit) {
    return Status::failure(ErrorCode::InvalidArgument,
                           std::string(name) + " must not exceed " + limit_name);
  }
  return Status::success();
}

Status require_le_u64(std::uint64_t value, std::uint64_t limit, const char* name,
                      const char* limit_name) {
  if (value > limit) {
    return Status::failure(ErrorCode::InvalidArgument,
                           std::string(name) + " must not exceed " + limit_name);
  }
  return Status::success();
}

}  // namespace

Status validate_limits(const IngestLimits& limits) {
  QOBS_TRY(require_positive_size(limits.max_batch_samples, "max_batch_samples"));
  QOBS_TRY(require_positive_size(limits.max_payload_bytes, "max_payload_bytes"));
  QOBS_TRY(require_positive_size(limits.max_field_bytes, "max_field_bytes"));
  QOBS_TRY(require_positive_size(limits.max_json_depth, "max_json_depth"));
  QOBS_TRY(require_positive_size(limits.max_json_nodes, "max_json_nodes"));
  QOBS_TRY(require_positive_size(limits.max_batch_queues, "max_batch_queues"));
  QOBS_TRY(require_positive_size(limits.max_batch_metadata, "max_batch_metadata"));
  QOBS_TRY(require_le(limits.max_json_depth, 128u, "max_json_depth", "128"));
  QOBS_TRY(require_le(limits.max_field_bytes, 1u << 20, "max_field_bytes", "1048576"));
  return Status::success();
}

Status validate_limits(const HistoryLimits& limits) {
  QOBS_TRY(require_positive_size(limits.max_queues, "max_queues"));
  QOBS_TRY(require_positive_size(limits.max_samples_per_queue, "max_samples_per_queue"));
  QOBS_TRY(require_positive_size(limits.max_events_per_queue, "max_events_per_queue"));
  QOBS_TRY(require_positive_size(limits.max_events_total, "max_events_total"));
  QOBS_TRY(require_positive_size(limits.max_windows_per_queue, "max_windows_per_queue"));
  QOBS_TRY(require_positive_size(limits.max_conflicts, "max_conflicts"));
  QOBS_TRY(require_positive_size(limits.max_burst_records, "max_burst_records"));
  QOBS_TRY(require_positive_size(limits.max_contention_records, "max_contention_records"));
  QOBS_TRY(require_positive_size(limits.max_sources, "max_sources"));
  QOBS_TRY(require_positive_size(limits.max_metadata_revisions, "max_metadata_revisions"));
  QOBS_TRY(require_positive(limits.max_history_bytes, "max_history_bytes"));
  QOBS_TRY(require_le(limits.max_events_per_queue, limits.max_events_total,
                      "max_events_per_queue", "max_events_total"));
  QOBS_TRY(require_le(limits.max_samples_per_queue, 1u << 20, "max_samples_per_queue",
                      "1048576"));
  return Status::success();
}

Status validate_limits(const QueryLimits& limits) {
  QOBS_TRY(require_positive_size(limits.max_rows, "max_rows"));
  QOBS_TRY(require_positive_size(limits.default_rows, "default_rows"));
  QOBS_TRY(require_positive_size(limits.max_query_windows, "max_query_windows"));
  QOBS_TRY(require_le(limits.default_rows, limits.max_rows, "default_rows", "max_rows"));
  QOBS_TRY(require_le(limits.max_query_windows, 32u, "max_query_windows", "32"));
  return Status::success();
}

Status validate_limits(const PersistenceLimits& limits) {
  QOBS_TRY(require_positive(limits.max_segment_bytes, "max_segment_bytes"));
  QOBS_TRY(require_positive(limits.max_segment_records, "max_segment_records"));
  QOBS_TRY(require_positive(limits.max_record_bytes, "max_record_bytes"));
  QOBS_TRY(require_positive_size(limits.max_segments, "max_segments"));
  QOBS_TRY(require_positive(limits.max_metadata_bytes, "max_metadata_bytes"));
  QOBS_TRY(require_le_u64(limits.max_record_bytes, limits.max_segment_bytes, "max_record_bytes",
                          "max_segment_bytes"));
  QOBS_TRY(require_le_u64(limits.max_metadata_bytes, limits.max_segment_bytes,
                          "max_metadata_bytes", "max_segment_bytes"));
  QOBS_TRY(require_le(limits.max_segments, 1024u, "max_segments", "1024"));
  return Status::success();
}

Status validate_limits(const RuntimeLimits& limits) {
  QOBS_TRY(require_positive_size(limits.max_pending_batches, "max_pending_batches"));
  QOBS_TRY(require_positive_size(limits.max_pending_samples, "max_pending_samples"));
  QOBS_TRY(require_positive_size(limits.max_deferred_records, "max_deferred_records"));
  QOBS_TRY(require_le(limits.worker_threads, 64u, "worker_threads", "64"));
  QOBS_TRY(require_le(limits.max_pending_batches, 1u << 20, "max_pending_batches", "1048576"));
  return Status::success();
}

Status validate_limits(const TransportLimits& limits) {
  QOBS_TRY(require_positive_size(limits.max_frame_bytes, "max_frame_bytes"));
  QOBS_TRY(require_positive_size(limits.max_connections, "max_connections"));
  QOBS_TRY(require_positive_size(limits.max_queued_frames, "max_queued_frames"));
  QOBS_TRY(require_positive_size(limits.max_connection_buffer_bytes,
                                 "max_connection_buffer_bytes"));
  QOBS_TRY(require_le(limits.max_connections, 4096u, "max_connections", "4096"));
  QOBS_TRY(require_le(limits.max_queued_frames, 1u << 16, "max_queued_frames", "65536"));
  return Status::success();
}

}  // namespace qobs
