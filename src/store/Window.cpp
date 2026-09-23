#include "qobs/store/Window.hpp"

#include <limits>

#include "qobs/core/Checked.hpp"

namespace qobs {
namespace {

constexpr std::size_t kNoBucket = std::numeric_limits<std::size_t>::max();

/// Saturating accumulation. Returns false when the sum overflowed, in which
/// case the accumulator is pinned at the maximum instead of wrapping.
bool accumulate(std::uint64_t& accumulator, std::uint64_t value) noexcept {
  const auto sum = checked::add_u64(accumulator, value);
  if (!sum.has_value()) {
    accumulator = std::numeric_limits<std::uint64_t>::max();
    return false;
  }
  accumulator = *sum;
  return true;
}

Nanos subtract_clamped(Nanos value, Nanos delta) noexcept {
  if (delta <= 0) {
    return value;
  }
  if (value < std::numeric_limits<Nanos>::min() + delta) {
    return std::numeric_limits<Nanos>::min();
  }
  return value - delta;
}

}  // namespace

Nanos align_bucket(Nanos value, Nanos bucket_ns) noexcept {
  if (bucket_ns <= 0) {
    return value;
  }
  Nanos quotient = value / bucket_ns;
  const Nanos remainder = value % bucket_ns;
  if (remainder < 0) {
    --quotient;
  }
  return quotient * bucket_ns;
}

Status validate_window_spec(const WindowSpec& spec) {
  if (spec.name.empty()) {
    return Status::failure(ErrorCode::InvalidArgument, "window spec requires a name");
  }
  if (spec.duration_ns <= 0) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "window duration must be a positive duration");
  }
  if (spec.bucket_ns <= 0) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "window bucket must be a positive duration");
  }
  if (spec.bucket_ns > spec.duration_ns) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "window bucket must not be longer than the window");
  }
  if (spec.max_buckets == 0u) {
    return Status::failure(ErrorCode::InvalidArgument, "window bucket budget must be non-zero");
  }
  const auto bucket_count = checked::div_ceil_u64(static_cast<std::uint64_t>(spec.duration_ns),
                                                  static_cast<std::uint64_t>(spec.bucket_ns));
  if (!bucket_count.has_value()) {
    return Status::failure(ErrorCode::Overflow, "window bucket count overflows");
  }
  if (*bucket_count > static_cast<std::uint64_t>(spec.max_buckets)) {
    return Status::failure(ErrorCode::LimitExceeded,
                           "window requires more buckets than the specification allows");
  }
  if (spec.min_covered_buckets == 0u) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "window requires at least one covered bucket");
  }
  if (static_cast<std::uint64_t>(spec.min_covered_buckets) > *bucket_count) {
    return Status::failure(ErrorCode::InvalidArgument,
                           "window coverage requirement exceeds its bucket count");
  }
  return Status::success();
}

Status BucketAccumulator::configure(WindowSpec spec) {
  QOBS_TRY(validate_window_spec(spec));
  const auto bucket_count = checked::div_ceil_u64(static_cast<std::uint64_t>(spec.duration_ns),
                                                  static_cast<std::uint64_t>(spec.bucket_ns));
  spec_ = std::move(spec);
  buckets_.assign(static_cast<std::size_t>(bucket_count.value()), WindowBucket{});
  size_ = 0;
  head_ = 0;
  evicted_ = 0;
  observations_ = 0;
  late_dropped_ = 0;
  configured_ = true;
  return Status::success();
}

void BucketAccumulator::clear() noexcept {
  for (WindowBucket& bucket : buckets_) {
    bucket = WindowBucket{};
  }
  size_ = 0;
  head_ = 0;
  evicted_ = 0;
  observations_ = 0;
  late_dropped_ = 0;
}

std::size_t BucketAccumulator::find_or_create_bucket(Nanos start_ns) {
  const std::size_t capacity = buckets_.size();
  if (capacity == 0u) {
    return kNoBucket;
  }
  for (std::size_t offset = 0; offset < size_; ++offset) {
    const std::size_t index = (head_ + size_ - 1u - offset) % capacity;
    if (buckets_[index].start_ns == start_ns) {
      return index;
    }
  }
  if (size_ > 0u && start_ns < buckets_[head_].start_ns) {
    // Older than anything retained: folding it in would break the ordering the
    // window relies on, so it is counted and dropped rather than misplaced.
    ++late_dropped_;
    return kNoBucket;
  }
  if (size_ < capacity) {
    const std::size_t index = (head_ + size_) % capacity;
    buckets_[index] = WindowBucket{};
    buckets_[index].start_ns = start_ns;
    ++size_;
    return index;
  }
  const std::size_t index = head_;
  buckets_[index] = WindowBucket{};
  buckets_[index].start_ns = start_ns;
  head_ = (head_ + 1u) % capacity;
  ++evicted_;
  return index;
}

void BucketAccumulator::expire(Nanos now_ns) {
  const std::size_t capacity = buckets_.size();
  if (capacity == 0u || size_ == 0u) {
    return;
  }
  const Nanos cutoff = subtract_clamped(now_ns, spec_.duration_ns);
  while (size_ > 0u && buckets_[head_].start_ns < cutoff) {
    buckets_[head_] = WindowBucket{};
    head_ = (head_ + 1u) % capacity;
    --size_;
    ++evicted_;
  }
}

void BucketAccumulator::observe(const WindowObservation& observation) {
  if (!configured_ || buckets_.empty()) {
    return;
  }
  ++observations_;
  expire(observation.steady_ns);
  const Nanos start = align_bucket(observation.steady_ns, spec_.bucket_ns);
  const std::size_t index = find_or_create_bucket(start);
  if (index == kNoBucket) {
    return;
  }
  WindowBucket& bucket = buckets_[index];
  ++bucket.sample_count;
  if (observation.has_occupancy) {
    if (!bucket.has_occupancy) {
      bucket.has_occupancy = true;
      bucket.occupancy_max = observation.occupancy;
      bucket.occupancy_min = observation.occupancy;
    } else {
      if (observation.occupancy > bucket.occupancy_max) {
        bucket.occupancy_max = observation.occupancy;
      }
      if (observation.occupancy < bucket.occupancy_min) {
        bucket.occupancy_min = observation.occupancy;
      }
    }
    bucket.occupancy_last = observation.occupancy;
  }
  const auto add_delta = [&bucket](std::uint64_t& target, const CounterDelta& delta) {
    if (delta.known) {
      if (!accumulate(target, delta.delta)) {
        ++bucket.saturation_events;
      }
    }
  };
  add_delta(bucket.drops, observation.drops);
  add_delta(bucket.marks, observation.marks);
  add_delta(bucket.pause_frames, observation.pause_frames);
  add_delta(bucket.pause_nanos, observation.pause_duration);
  add_delta(bucket.enqueues, observation.enqueues);
  add_delta(bucket.dequeues, observation.dequeues);
  if (observation.any_delta_unknown) {
    ++bucket.unknown_delta_samples;
  }
  if (!observation.establishes_current) {
    ++bucket.non_current_samples;
  }
  if (observation.steady_ns > bucket.last_received_ns) {
    bucket.last_received_ns = observation.steady_ns;
  }
}

std::vector<WindowBucket> BucketAccumulator::buckets() const {
  std::vector<WindowBucket> out;
  out.reserve(size_);
  const std::size_t capacity = buckets_.size();
  for (std::size_t offset = 0; offset < size_; ++offset) {
    out.push_back(buckets_[(head_ + offset) % capacity]);
  }
  return out;
}

WindowAggregate BucketAccumulator::aggregate(Nanos now_ns, const FreshnessPolicy& freshness) const {
  WindowAggregate aggregate;
  aggregate.spec = spec_;
  aggregate.window_end_ns = now_ns;
  aggregate.window_start_ns = subtract_clamped(now_ns, spec_.duration_ns);
  aggregate.bucket_count = size_;

  const std::size_t capacity = buckets_.size();
  bool has_occupancy = false;
  std::uint64_t occupancy_max = 0;
  std::uint64_t occupancy_min = 0;
  std::uint64_t occupancy_last = 0;
  Nanos newest_received = std::numeric_limits<Nanos>::min();
  bool newest_received_set = false;

  for (std::size_t offset = 0; offset < size_; ++offset) {
    const WindowBucket& bucket = buckets_[(head_ + offset) % capacity];
    if (bucket.sample_count == 0u) {
      continue;
    }
    ++aggregate.covered_buckets;
    aggregate.sample_count += bucket.sample_count;
    aggregate.drops += bucket.drops;
    aggregate.marks += bucket.marks;
    aggregate.pause_frames += bucket.pause_frames;
    aggregate.pause_nanos += bucket.pause_nanos;
    aggregate.enqueues += bucket.enqueues;
    aggregate.dequeues += bucket.dequeues;
    aggregate.unknown_delta_samples += bucket.unknown_delta_samples;
    aggregate.non_current_samples += bucket.non_current_samples;
    aggregate.saturation_events += bucket.saturation_events;
    if (bucket.has_occupancy) {
      if (!has_occupancy) {
        has_occupancy = true;
        occupancy_max = bucket.occupancy_max;
        occupancy_min = bucket.occupancy_min;
      } else {
        if (bucket.occupancy_max > occupancy_max) {
          occupancy_max = bucket.occupancy_max;
        }
        if (bucket.occupancy_min < occupancy_min) {
          occupancy_min = bucket.occupancy_min;
        }
      }
      occupancy_last = bucket.occupancy_last;
    }
    if (bucket.last_received_ns > 0 && (!newest_received_set || bucket.last_received_ns > newest_received)) {
      newest_received = bucket.last_received_ns;
      newest_received_set = true;
    }
  }

  if (has_occupancy) {
    aggregate.occupancy_max = occupancy_max;
    aggregate.occupancy_min = occupancy_min;
    aggregate.occupancy_last = occupancy_last;
    aggregate.occupancy_excursion = occupancy_max - occupancy_min;
  }

  const bool coverage_ok = aggregate.covered_buckets >= spec_.min_covered_buckets;
  aggregate.coverage_sufficient = coverage_ok;

  EvidenceAssessment assessment;
  assessment.provenance = Provenance::Correlated;
  if (aggregate.covered_buckets == 0u) {
    assessment.quality = EvidenceQuality::Unknown;
    assessment.freshness = Freshness::Unknown;
    assessment.reason = "the window contains no observations";
  } else {
    const Nanos age = newest_received_set ? now_ns - newest_received : 0;
    assessment.age_ns = age < 0 ? 0 : age;
    assessment.freshness = assess_freshness(age, freshness);
    if (!coverage_ok) {
      assessment.quality = EvidenceQuality::Incomplete;
      assessment.flags = with_flag(assessment.flags, EvidenceFlag::CoverageInsufficient);
      assessment.reason = "only " + std::to_string(aggregate.covered_buckets) +
                          " of the required " + std::to_string(spec_.min_covered_buckets) +
                          " buckets contain observations";
    } else if (aggregate.unknown_delta_samples > 0u) {
      assessment.quality = EvidenceQuality::Incomplete;
      assessment.flags = with_flag(assessment.flags, EvidenceFlag::CounterDiscontinuity);
      assessment.reason = std::to_string(aggregate.unknown_delta_samples) +
                          " samples in the window had no established counter delta";
    } else if (aggregate.non_current_samples > 0u) {
      assessment.quality = EvidenceQuality::Incomplete;
      assessment.flags = with_flag(assessment.flags, EvidenceFlag::Recovered);
      assessment.reason = std::to_string(aggregate.non_current_samples) +
                          " samples in the window cannot speak for the present";
    } else {
      assessment.quality = EvidenceQuality::Complete;
    }
  }
  aggregate.assessment = std::move(assessment);
  return aggregate;
}

BucketAccumulator::BurstEvidence BucketAccumulator::burst_evidence(
    std::uint64_t min_excursion, std::uint32_t min_covered_buckets) const {
  BurstEvidence evidence;
  evidence.evaluated = true;
  evidence.required_excursion = min_excursion;
  evidence.required_buckets = min_covered_buckets;

  const std::size_t capacity = buckets_.size();
  bool seen = false;
  std::uint64_t peak = 0;
  std::uint64_t trough = 0;
  Nanos peak_at = 0;
  Nanos trough_at = 0;
  std::size_t covered = 0;

  for (std::size_t offset = 0; offset < size_; ++offset) {
    const WindowBucket& bucket = buckets_[(head_ + offset) % capacity];
    if (bucket.sample_count == 0u || !bucket.has_occupancy) {
      continue;
    }
    ++covered;
    if (!seen) {
      seen = true;
      peak = bucket.occupancy_max;
      trough = bucket.occupancy_min;
      peak_at = bucket.start_ns;
      trough_at = bucket.start_ns;
      continue;
    }
    if (bucket.occupancy_max > peak) {
      peak = bucket.occupancy_max;
      peak_at = bucket.start_ns;
    }
    if (bucket.occupancy_min < trough) {
      trough = bucket.occupancy_min;
      trough_at = bucket.start_ns;
    }
  }

  evidence.covered_buckets = covered;
  evidence.peak = peak;
  evidence.trough = trough;
  evidence.peak_at_ns = peak_at;
  evidence.trough_at_ns = trough_at;
  evidence.excursion = seen ? peak - trough : 0;
  evidence.coverage_sufficient = covered >= min_covered_buckets;
  evidence.detected = evidence.coverage_sufficient && evidence.excursion >= min_excursion;
  // Sub-sample behaviour is never reconstructed. This is a hard boundary of the
  // runtime, not a configuration option.
  evidence.sub_sample_reconstructed = false;
  return evidence;
}

}  // namespace qobs
