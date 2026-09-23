#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "qobs/core/Result.hpp"
#include "qobs/core/Time.hpp"
#include "qobs/model/Counter.hpp"
#include "qobs/model/Evidence.hpp"
#include "qobs/model/Sample.hpp"

namespace qobs {

/// A named, bounded aggregation window.
struct WindowSpec {
  std::string name{};
  Nanos duration_ns{0};
  Nanos bucket_ns{0};
  /// Buckets that must contain at least one sample before the window is
  /// considered to have enough coverage to support a conclusion.
  std::uint32_t min_covered_buckets{1};
  /// Hard cap on buckets; a spec that would need more is rejected.
  std::size_t max_buckets{64};

  friend bool operator==(const WindowSpec&, const WindowSpec&) = default;
};

/// Validate a window specification. Checked arithmetic is used for the bucket
/// count so that a hostile duration cannot overflow the bound.
[[nodiscard]] Status validate_window_spec(const WindowSpec& spec);

/// One bucket of aggregated evidence. Counters are summed only across samples
/// whose delta was established; unknown deltas are counted separately and never
/// contribute.
struct WindowBucket {
  Nanos start_ns{0};
  std::size_t sample_count{0};
  bool has_occupancy{false};
  std::uint64_t occupancy_max{0};
  std::uint64_t occupancy_min{0};
  std::uint64_t occupancy_last{0};
  std::uint64_t drops{0};
  std::uint64_t marks{0};
  std::uint64_t pause_frames{0};
  std::uint64_t pause_nanos{0};
  std::uint64_t enqueues{0};
  std::uint64_t dequeues{0};
  std::uint64_t unknown_delta_samples{0};
  std::uint64_t non_current_samples{0};
  /// Number of additions that saturated because the sum would have overflowed.
  std::uint64_t saturation_events{0};
  /// Steady receive time of the newest sample folded into this bucket.
  Nanos last_received_ns{0};
};

/// A single observation folded into an accumulator.
struct WindowObservation {
  Nanos steady_ns{0};
  bool has_occupancy{false};
  std::uint64_t occupancy{0};
  CounterDelta drops{};
  CounterDelta marks{};
  CounterDelta pause_frames{};
  CounterDelta pause_duration{};
  CounterDelta enqueues{};
  CounterDelta dequeues{};
  /// True when the evidence that produced this observation is allowed to speak
  /// for the present.
  bool establishes_current{false};
  /// True when at least one contributing delta was not established.
  bool any_delta_unknown{false};
};

/// Aggregated view of one window.
struct WindowAggregate {
  WindowSpec spec{};
  Nanos window_start_ns{0};
  Nanos window_end_ns{0};
  std::size_t bucket_count{0};
  std::size_t covered_buckets{0};
  std::size_t sample_count{0};
  std::optional<std::uint64_t> occupancy_max{};
  std::optional<std::uint64_t> occupancy_min{};
  std::optional<std::uint64_t> occupancy_last{};
  std::uint64_t occupancy_excursion{0};
  std::uint64_t drops{0};
  std::uint64_t marks{0};
  std::uint64_t pause_frames{0};
  std::uint64_t pause_nanos{0};
  std::uint64_t enqueues{0};
  std::uint64_t dequeues{0};
  std::uint64_t unknown_delta_samples{0};
  std::uint64_t non_current_samples{0};
  std::uint64_t saturation_events{0};
  Nanos newest_received_ns{0};
  bool coverage_sufficient{false};
  EvidenceAssessment assessment{};
};

/// Fixed-capacity bucketed accumulator.
///
/// The accumulator never grows past its bucket budget. Once the budget is
/// reached the oldest bucket is evicted and the eviction is counted, so an
/// under-covered window is reported as such instead of being silently widened.
class BucketAccumulator {
 public:
  BucketAccumulator() = default;

  [[nodiscard]] Status configure(WindowSpec spec);
  [[nodiscard]] const WindowSpec& spec() const noexcept { return spec_; }
  [[nodiscard]] bool configured() const noexcept { return configured_; }

  /// Fold one observation in, evicting buckets that fell out of the window.
  void observe(const WindowObservation& observation);

  /// Drop buckets older than the window relative to the supplied time.
  void expire(Nanos now_ns);

  void clear() noexcept;

  [[nodiscard]] std::size_t bucket_count() const noexcept { return size_; }
  [[nodiscard]] std::uint64_t evicted_buckets() const noexcept { return evicted_; }
  /// Observations that arrived older than the oldest retained bucket and were
  /// therefore not foldable into an ordered window.
  [[nodiscard]] std::uint64_t late_dropped() const noexcept { return late_dropped_; }
  [[nodiscard]] std::uint64_t observations() const noexcept { return observations_; }

  /// Buckets in ascending time order. Copying is deliberate: callers must not
  /// hold a reference into accumulator storage.
  [[nodiscard]] std::vector<WindowBucket> buckets() const;

  /// Aggregate the current contents. The freshness policy is applied to the
  /// newest sample's receive time so that an aggregate is never reported as
  /// fresher than the evidence underneath it.
  [[nodiscard]] WindowAggregate aggregate(Nanos now_ns,
                                          const FreshnessPolicy& freshness) const;

  /// Microburst evidence derived from the covered buckets of this window.
  struct BurstEvidence {
    bool evaluated{false};
    bool detected{false};
    bool coverage_sufficient{false};
    std::uint64_t peak{0};
    std::uint64_t trough{0};
    std::uint64_t excursion{0};
    std::uint64_t required_excursion{0};
    std::size_t covered_buckets{0};
    std::size_t required_buckets{0};
    Nanos peak_at_ns{0};
    Nanos trough_at_ns{0};
    /// Always false: this runtime never reconstructs behaviour between
    /// samples. The field exists so that a consumer can assert the boundary.
    bool sub_sample_reconstructed{false};
  };

  /// Peak-to-trough evidence across the covered buckets only.
  [[nodiscard]] BurstEvidence burst_evidence(std::uint64_t min_excursion,
                                             std::uint32_t min_covered_buckets) const;

 private:
  [[nodiscard]] std::size_t find_or_create_bucket(Nanos start_ns);

  WindowSpec spec_{};
  bool configured_{false};
  std::vector<WindowBucket> buckets_{};
  std::size_t size_{0};
  std::size_t head_{0};
  std::uint64_t evicted_{0};
  std::uint64_t observations_{0};
  std::uint64_t late_dropped_{0};
};

/// Align a steady timestamp down to the start of its bucket.
[[nodiscard]] Nanos align_bucket(Nanos value, Nanos bucket_ns) noexcept;

}  // namespace qobs
