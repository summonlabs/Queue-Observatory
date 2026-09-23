#pragma once

#include <cstddef>
#include <cstdint>

#include "qobs/core/Result.hpp"

namespace qobs {

/// Every unbounded input in the runtime is bounded here. Limits are part of the
/// public configuration surface and are validated on construction.
struct IngestLimits {
  /// Maximum samples accepted in a single ingest batch.
  std::size_t max_batch_samples{4096};
  /// Maximum encoded batch payload in bytes.
  std::size_t max_payload_bytes{8u * 1024u * 1024u};
  /// Maximum length of a single string field on the wire.
  std::size_t max_field_bytes{4096};
  /// JSON structural limits applied to a decoded document.
  std::size_t max_json_depth{24};
  std::size_t max_json_nodes{200000};
  /// Maximum number of distinct queues a single batch may introduce.
  std::size_t max_batch_queues{4096};
  /// Maximum number of metadata records in one batch.
  std::size_t max_batch_metadata{1024};

  friend bool operator==(const IngestLimits&, const IngestLimits&) = default;
};

struct HistoryLimits {
  /// Maximum number of distinct queues tracked at once. Exceeding this is a
  /// reported LimitExceeded, never a silent eviction of live state.
  std::size_t max_queues{8192};
  /// Ring capacity of the occupancy history of one queue.
  std::size_t max_samples_per_queue{256};
  /// Total byte budget for retained history across every queue. A queue whose
  /// ring would push the total past this budget is rejected with an explicit
  /// LimitExceeded instead of silently shrinking another queue's history.
  std::uint64_t max_history_bytes{128u * 1024u * 1024u};
  /// Ring capacity of the event log of one queue.
  std::size_t max_events_per_queue{512};
  /// Maximum events retained across all queues.
  std::size_t max_events_total{65536};
  /// Maximum aggregation windows retained per queue.
  std::size_t max_windows_per_queue{8};
  /// Maximum conflict records retained.
  std::size_t max_conflicts{4096};
  /// Maximum live microburst evidence records.
  std::size_t max_burst_records{4096};
  /// Maximum sibling contention records.
  std::size_t max_contention_records{4096};
  /// Maximum sources registered.
  std::size_t max_sources{512};
  /// Maximum metadata revisions retained per class table.
  std::size_t max_metadata_revisions{64};

  friend bool operator==(const HistoryLimits&, const HistoryLimits&) = default;
};

struct QueryLimits {
  /// Hard ceiling on rows returned by a single query.
  std::size_t max_rows{100000};
  /// Default page size when a query does not specify one.
  std::size_t default_rows{1000};
  /// Hard ceiling on the number of windows a history query may request.
  std::size_t max_query_windows{8};

  friend bool operator==(const QueryLimits&, const QueryLimits&) = default;
};

struct PersistenceLimits {
  /// Maximum size of a single persistence segment.
  std::uint64_t max_segment_bytes{64u * 1024u * 1024u};
  /// Maximum records in a single persistence segment.
  std::uint64_t max_segment_records{1000000u};
  /// Maximum value bytes in a single encoded record.
  std::uint64_t max_record_bytes{1024u * 1024u};
  /// Maximum number of retained segments.
  std::size_t max_segments{8};
  /// Maximum serialized metadata blob.
  std::uint64_t max_metadata_bytes{1u * 1024u * 1024u};

  friend bool operator==(const PersistenceLimits&, const PersistenceLimits&) = default;
};

struct RuntimeLimits {
  /// Maximum batches waiting to be applied by the background applier.
  std::size_t max_pending_batches{256};
  /// Maximum samples waiting in the pending queue.
  std::size_t max_pending_samples{262144};
  /// Number of background worker threads. Zero selects a synchronous runtime.
  std::size_t worker_threads{2};
  /// Maximum queued records held while a batch is being applied.
  std::size_t max_deferred_records{4096};

  friend bool operator==(const RuntimeLimits&, const RuntimeLimits&) = default;
};

struct TransportLimits {
  /// Maximum accepted frame body.
  std::size_t max_frame_bytes{8u * 1024u * 1024u};
  /// Maximum concurrent client connections.
  std::size_t max_connections{32};
  /// Maximum queued frames per connection.
  std::size_t max_queued_frames{64};
  /// Bound on the number of bytes buffered per connection while decoding.
  std::size_t max_connection_buffer_bytes{16u * 1024u * 1024u};

  friend bool operator==(const TransportLimits&, const TransportLimits&) = default;
};

/// Validate that a limits structure is internally coherent. Called once when a
/// component is constructed so that an impossible configuration fails loudly at
/// startup rather than mid-operation.
[[nodiscard]] Status validate_limits(const IngestLimits& limits);
[[nodiscard]] Status validate_limits(const HistoryLimits& limits);
[[nodiscard]] Status validate_limits(const QueryLimits& limits);
[[nodiscard]] Status validate_limits(const PersistenceLimits& limits);
[[nodiscard]] Status validate_limits(const RuntimeLimits& limits);
[[nodiscard]] Status validate_limits(const TransportLimits& limits);

}  // namespace qobs
