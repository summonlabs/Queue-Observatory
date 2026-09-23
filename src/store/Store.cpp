#include "qobs/store/Store.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "qobs/core/Checked.hpp"
#include "qobs/core/Json.hpp"
#include "qobs/core/LockAudit.hpp"
#include "qobs/core/Text.hpp"
#include "qobs/policy/Explanation.hpp"

namespace qobs {
namespace {

constexpr std::size_t kMaxFailureDetails = 64u;
constexpr std::size_t kMaxRecentIncarnations = 4u;
constexpr std::size_t kEvaluationBuckets = 8u;
constexpr std::size_t kContentionBuckets = 8u;

const CounterDelta& empty_delta() {
  static const CounterDelta kEmpty{};
  return kEmpty;
}

WindowSpec make_spec(std::string name, Nanos duration_ns, Nanos bucket_ns,
                     std::uint32_t min_covered, std::size_t max_buckets) {
  WindowSpec spec;
  spec.name = std::move(name);
  spec.duration_ns = duration_ns;
  spec.bucket_ns = bucket_ns;
  spec.min_covered_buckets = min_covered;
  spec.max_buckets = max_buckets;
  return spec;
}

/// Sum of established counter deltas over the retained records at or after the
/// cutoff. Records whose deltas were not established set the unknown flag; they
/// never contribute a value.
struct WindowSums {
  CounterDelta drops{};
  CounterDelta marks{};
  CounterDelta pause_frames{};
  CounterDelta pause_duration{};
  CounterDelta enqueues{};
  CounterDelta dequeues{};
  bool any_unknown{false};
  std::size_t considered{0};
};

void accumulate_delta(CounterDelta& target, const CounterDelta& value) {
  if (!value.known) {
    return;
  }
  if (!target.known) {
    target.known = true;
    target.delta = 0;
    target.continuity = CounterContinuity::Continuous;
  }
  const auto sum = checked::add_u64(target.delta, value.delta);
  target.delta = sum.has_value() ? *sum : std::numeric_limits<std::uint64_t>::max();
}

void accumulate_field(WindowSums& sums, const HistoryRecord& record, SampleField field,
                      CounterDelta& target) {
  const CounterDelta& delta = record.delta(field);
  if (!has_field(record.reported, field)) {
    // The source did not report this counter in this sample. The sum is not
    // complete, but no value is invented for it.
    sums.any_unknown = true;
    return;
  }
  if (!delta.known) {
    sums.any_unknown = true;
    return;
  }
  accumulate_delta(target, delta);
}

WindowSums window_sums(const HistoryRing& history, Nanos cutoff_ns) {
  WindowSums sums;
  for (std::size_t offset = 0; offset < history.size(); ++offset) {
    const HistoryRecord& record = history.at(history.size() - 1u - offset);
    if (record.received.steady.ns < cutoff_ns) {
      break;
    }
    ++sums.considered;
    if (!record.establishes_current()) {
      sums.any_unknown = true;
    }
    accumulate_field(sums, record, SampleField::DropPackets, sums.drops);
    accumulate_field(sums, record, SampleField::MarkPackets, sums.marks);
    accumulate_field(sums, record, SampleField::PauseFrames, sums.pause_frames);
    accumulate_field(sums, record, SampleField::PauseDurationNanos, sums.pause_duration);
    accumulate_field(sums, record, SampleField::EnqueuePackets, sums.enqueues);
    accumulate_field(sums, record, SampleField::DequeuePackets, sums.dequeues);
  }
  return sums;
}

WindowObservation to_observation(const HistoryRecord& record) {
  WindowObservation observation;
  observation.steady_ns = record.received.steady.ns;
  if (const auto occupancy = record.value(SampleField::OccupancyCells); occupancy.has_value()) {
    observation.has_occupancy = true;
    observation.occupancy = *occupancy;
  } else if (const auto bytes = record.value(SampleField::OccupancyBytes); bytes.has_value()) {
    observation.has_occupancy = true;
    observation.occupancy = *bytes;
  }
  observation.drops = record.delta(SampleField::DropPackets);
  observation.marks = record.delta(SampleField::MarkPackets);
  observation.pause_frames = record.delta(SampleField::PauseFrames);
  observation.pause_duration = record.delta(SampleField::PauseDurationNanos);
  observation.enqueues = record.delta(SampleField::EnqueuePackets);
  observation.dequeues = record.delta(SampleField::DequeuePackets);
  observation.establishes_current = record.establishes_current();
  observation.any_delta_unknown =
      !observation.drops.known || !observation.marks.known || !observation.pause_frames.known ||
      !observation.pause_duration.known || !observation.enqueues.known ||
      !observation.dequeues.known;
  return observation;
}

std::string describe_missing(FieldMask mask) {
  std::string out;
  for (std::size_t index = 0; index < kSampleFieldCount; ++index) {
    const auto field = static_cast<SampleField>(index);
    if (!has_field(mask, field)) {
      continue;
    }
    if (!out.empty()) {
      out.push_back(',');
    }
    out.append(to_string(field));
  }
  if (out.empty()) {
    out.append("none");
  }
  return out;
}

}  // namespace

std::string_view to_string(AdmissionOutcome outcome) noexcept {
  switch (outcome) {
    case AdmissionOutcome::Accepted:
      return "accepted";
    case AdmissionOutcome::AcceptedNewSource:
      return "accepted_new_source";
    case AdmissionOutcome::AcceptedNewIncarnation:
      return "accepted_new_incarnation";
    case AdmissionOutcome::AcceptedNewGeneration:
      return "accepted_new_generation";
    case AdmissionOutcome::AcceptedLateButFresh:
      return "accepted_late_but_fresh";
    case AdmissionOutcome::RejectedInvalid:
      return "rejected_invalid";
    case AdmissionOutcome::RejectedLimit:
      return "rejected_limit";
    case AdmissionOutcome::RejectedNotRunning:
      return "rejected_not_running";
    case AdmissionOutcome::FencedStaleIncarnation:
      return "fenced_stale_incarnation";
    case AdmissionOutcome::FencedStaleSequence:
      return "fenced_stale_sequence";
    case AdmissionOutcome::FencedStaleGeneration:
      return "fenced_stale_generation";
    case AdmissionOutcome::FencedAuthorityDowngrade:
      return "fenced_authority_downgrade";
  }
  return "unmapped";
}

bool outcome_admitted(AdmissionOutcome outcome) noexcept {
  switch (outcome) {
    case AdmissionOutcome::Accepted:
    case AdmissionOutcome::AcceptedNewSource:
    case AdmissionOutcome::AcceptedNewIncarnation:
    case AdmissionOutcome::AcceptedNewGeneration:
    case AdmissionOutcome::AcceptedLateButFresh:
      return true;
    default:
      return false;
  }
}

std::string_view fence_reason(AdmissionOutcome outcome) noexcept {
  switch (outcome) {
    case AdmissionOutcome::FencedStaleIncarnation:
      return "the sample names an incarnation that this runtime has already superseded";
    case AdmissionOutcome::FencedStaleSequence:
      return "the sample sequence is not strictly newer than the last accepted sequence";
    case AdmissionOutcome::FencedStaleGeneration:
      return "the sample names a device generation older than the current one";
    case AdmissionOutcome::FencedAuthorityDowngrade:
      return "the source declared a lower authority than it previously established";
    default:
      return "";
  }
}

std::string make_runtime_id(std::uint64_t seed) {
  return "qobs-" + text::to_hex(seed, 16u);
}

QueueStore::QueueStore(PressurePolicy policy, HistoryLimits history, QueryLimits query,
                       std::shared_ptr<Clock> clock)
    : policy_(std::move(policy)),
      history_limits_(history),
      query_limits_(query),
      clock_(std::move(clock)) {
  session_.started = clock_ != nullptr ? clock_->now() : receive_now();
  session_.runtime_id =
      make_runtime_id(static_cast<std::uint64_t>(session_.started.wall.ns));
  session_.policy_version = policy_.version;
  session_.policy_name = policy_.name;
  session_.runtime_version = version();

  event_log_.assign(history_limits_.max_events_total, EventRecord{});

  const auto bucket_for = [](Nanos duration, std::size_t buckets) -> Nanos {
    if (buckets == 0u || duration <= 0) {
      return duration;
    }
    const Nanos bucket = duration / static_cast<Nanos>(buckets);
    return bucket <= 0 ? duration : bucket;
  };

  evaluation_spec_ = make_spec("evaluation", policy_.evaluation_window_ns,
                               bucket_for(policy_.evaluation_window_ns, kEvaluationBuckets), 1u,
                               kEvaluationBuckets);
  microburst_spec_ = make_spec("microburst", policy_.microburst.window_ns,
                               policy_.microburst.bucket_ns, policy_.microburst.min_covered_buckets,
                               policy_.microburst.max_buckets);
  contention_spec_ =
      make_spec("contention", policy_.contention.window_ns,
                bucket_for(policy_.contention.window_ns, kContentionBuckets),
                policy_.contention.min_covered_buckets, kContentionBuckets);

  config_status_ = validate_limits(history_limits_);
  if (config_status_.ok()) {
    config_status_ = validate_limits(query_limits_);
  }
  if (config_status_.ok()) {
    config_status_ = validate_policy(policy_);
  }
  if (config_status_.ok()) {
    config_status_ = validate_window_spec(evaluation_spec_);
  }
  if (config_status_.ok()) {
    config_status_ = validate_window_spec(microburst_spec_);
  }
  if (config_status_.ok()) {
    config_status_ = validate_window_spec(contention_spec_);
  }
}

QueueStore::~QueueStore() = default;

void QueueStore::record_event(EventKind kind, Severity severity, const QueueSample* sample,
                              const QueuePath& queue, std::string detail) {
  record_event_global(kind, severity, std::move(detail));
  if (event_size_ == 0u) {
    return;
  }
  EventRecord& record = event_log_[(event_head_ + event_size_ - 1u) % event_log_.size()];
  record.queue = queue;
  if (sample != nullptr) {
    record.observed = sample->observed;
    record.source = sample->source;
    record.incarnation = sample->incarnation;
    record.sequence = sample->sequence;
    record.revision = sample->revision;
  }
}

void QueueStore::record_event_global(EventKind kind, Severity severity, std::string detail) {
  if (event_log_.empty()) {
    return;
  }
  EventRecord record;
  record.kind = kind;
  record.severity = severity;
  record.received = clock_ != nullptr ? clock_->now() : receive_now();
  record.detail = std::move(detail);
  if (event_size_ < event_log_.size()) {
    event_log_[(event_head_ + event_size_) % event_log_.size()] = std::move(record);
    ++event_size_;
  } else {
    event_log_[event_head_] = std::move(record);
    event_head_ = (event_head_ + 1u) % event_log_.size();
    ++counters_.events_evicted;
  }
  ++counters_.events_recorded;
}

QueueStore::QueueState& QueueStore::acquire_queue(const QueuePath& queue, bool& created) {
  const auto existing = queues_.find(queue);
  if (existing != queues_.end()) {
    created = false;
    return existing->second;
  }
  created = true;
  QueueState state;
  state.queue = queue;
  queues_.emplace(queue, std::move(state));
  QueueState& stored = queues_.find(queue)->second;
  const std::uint64_t per_queue =
      static_cast<std::uint64_t>(history_limits_.max_samples_per_queue) *
      static_cast<std::uint64_t>(sizeof(HistoryRecord));
  const auto needed = checked::add_u64(history_bytes_, per_queue);
  if (needed.has_value()) {
    stored.history.reserve(history_limits_.max_samples_per_queue);
    history_bytes_ = *needed;
  }
  return stored;
}

EvidenceAssessment QueueStore::assess(const QueueSample& sample, const SourceRecord& source,
                                      bool metadata_derived) const {
  EvidenceAssessment assessment;
  const ReceiveTime now = clock_ != nullptr ? clock_->now() : receive_now();
  const Nanos age = now.steady.ns - sample.received.steady.ns;
  assessment.age_ns = age < 0 ? 0 : age;
  assessment.freshness = assess_freshness(age, policy_.freshness);
  assessment.authority = source.authority;
  if (recovered_mode_) {
    // Evidence read back from persistence is never allowed to look current.
    // Whatever the age computation produced, the recovery path caps it, so a
    // restart can never silently promote old evidence into a live state.
    assessment.provenance = Provenance::RecoveredFromPersistence;
    if (assessment.freshness == Freshness::Fresh || assessment.freshness == Freshness::Unknown) {
      assessment.freshness = Freshness::Stale;
    }
  } else {
    assessment.provenance =
        metadata_derived ? Provenance::DerivedFromMetadata : Provenance::Observed;
  }

  EvidenceFlags flags = 0;
  if (age < 0) {
    flags = with_flag(flags, EvidenceFlag::OrderAnomaly);
  }
  if (!sample.observed.known()) {
    flags = with_flag(flags, EvidenceFlag::ClockDomainUnknown);
  } else if (source.clock_domain.valid() && source.clock_domain != sample.observed.domain) {
    flags = with_flag(flags, EvidenceFlag::ClockDomainMismatch);
  }
  if (metadata_derived) {
    flags = with_flag(flags, EvidenceFlag::ClassResolvedFromMetadata);
  }
  if (recovered_mode_) {
    flags = with_flag(flags, EvidenceFlag::Recovered);
  }
  assessment.flags = flags;

  const FieldMask missing = policy_.required_fields & ~sample.reported;
  if (missing != 0u) {
    assessment.quality = EvidenceQuality::Incomplete;
    assessment.reason = "required fields were not reported: " + describe_missing(missing);
  } else {
    assessment.quality = EvidenceQuality::Complete;
  }
  if (recovered_mode_) {
    assessment.quality = EvidenceQuality::Incomplete;
    assessment.reason =
        "evidence was read back from persistence and cannot define the current state";
  }
  return assessment;
}

Status QueueStore::admit_recovered(const std::vector<QueueSample>& samples,
                                   BatchAdmission& result) {
  result = BatchAdmission{};
  if (!config_status_.ok()) {
    return config_status_;
  }
  std::unique_lock<std::shared_mutex> lock(mutex_);
  LockRankGuard guard(kRankStore, "store");
  recovered_mode_ = true;
  for (std::size_t index = 0; index < samples.size(); ++index) {
    AdmissionResult admission;
    const Status status = ingest_locked(samples[index], admission);
    result.presented += 1u;
    if (!status.ok()) {
      result.rejected += 1u;
      if (result.failures.size() < kMaxFailureDetails) {
        AdmissionFailure failure;
        failure.index = index;
        failure.outcome = admission.outcome;
        failure.queue = admission.queue;
        failure.reason = admission.reason.empty() ? status.message() : admission.reason;
        result.failures.push_back(std::move(failure));
      } else {
        result.failures_truncated = true;
      }
    } else if (outcome_admitted(admission.outcome)) {
      result.accepted += 1u;
    } else {
      result.fenced += 1u;
      if (admission.outcome == AdmissionOutcome::FencedStaleSequence) {
        result.duplicates += 1u;
      }
    }
    if (admission.conflict_detected) {
      result.conflicts += 1u;
    }
    if (admission.queue_created) {
      result.queues_created += 1u;
    }
  }
  recovered_mode_ = false;
  return Status::success();
}

Status QueueStore::import_events(const std::vector<EventRecord>& events, std::size_t& imported) {
  imported = 0;
  if (!config_status_.ok()) {
    return config_status_;
  }
  std::unique_lock<std::shared_mutex> lock(mutex_);
  LockRankGuard guard(kRankStore, "store");
  for (const EventRecord& event : events) {
    if (event_size_ >= history_limits_.max_events_total) {
      break;
    }
    record_event_global(event.kind, event.severity, event.detail);
    if (event_size_ == 0u) {
      break;
    }
    EventRecord& stored = event_log_[(event_head_ + event_size_ - 1u) % event_log_.size()];
    stored.received = event.received;
    stored.observed = event.observed;
    stored.queue = event.queue;
    stored.source = event.source;
    stored.incarnation = event.incarnation;
    stored.sequence = event.sequence;
    stored.revision = event.revision;
    ++imported;
  }
  return Status::success();
}

std::uint64_t QueueStore::resolve_conflict_tolerance(std::uint64_t value_a,
                                                     std::uint64_t value_b) const {
  const std::uint64_t larger = std::max(value_a, value_b);
  std::uint64_t tolerance = policy_.conflict.absolute_tolerance;
  const auto relative = permille_of(larger, policy_.conflict.relative_permille);
  if (relative.has_value() && relative.value() > tolerance) {
    tolerance = relative.value();
  }
  return tolerance;
}

void QueueStore::detect_conflict(QueueState& state, const QueueSample& sample,
                                 const SourceRecord& source, AdmissionResult& result) {
  if (!policy_.conflict.enabled) {
    return;
  }
  const auto occupancy = sample.occupancy_for_pressure();
  const SampleField occupancy_field = sample.has(SampleField::OccupancyCells)
                                          ? SampleField::OccupancyCells
                                          : SampleField::OccupancyBytes;
  const Nanos now_ns = sample.received.steady.ns;

  bool conflict = false;
  SourceId other_source{};
  std::uint64_t other_value = 0;
  std::uint64_t difference = 0;
  std::uint64_t tolerance = 0;

  if (occupancy.has_value()) {
    for (const PeerObservation& peer : state.peers) {
      if (peer.source == sample.source || !peer.has_occupancy) {
        continue;
      }
      if (peer.occupancy_field != occupancy_field) {
        continue;
      }
      if (now_ns - peer.received_ns > policy_.conflict.window_ns ||
          peer.received_ns > now_ns) {
        continue;
      }
      // Only equally authoritative sources can conflict. When one source is
      // strictly more authoritative its value stands, and the divergence is
      // not a conflict but a known authority relationship.
      if (peer.authority != source.authority) {
        continue;
      }
      const std::uint64_t observed_difference =
          *occupancy > peer.occupancy ? *occupancy - peer.occupancy : peer.occupancy - *occupancy;
      const std::uint64_t observed_tolerance =
          resolve_conflict_tolerance(*occupancy, peer.occupancy);
      if (observed_difference > observed_tolerance) {
        conflict = true;
        other_source = peer.source;
        other_value = peer.occupancy;
        difference = observed_difference;
        tolerance = observed_tolerance;
        break;
      }
    }
  }

  if (conflict && !state.conflict_active) {
    state.conflict_active = true;
    state.conflict_since = sample.received;
    state.conflicts += 1u;
    counters_.conflicts_detected += 1u;
    ConflictRecord record;
    record.queue = state.queue;
    record.detected = sample.received;
    record.source_a = sample.source;
    record.source_b = other_source;
    record.value_a = occupancy.value_or(0);
    record.value_b = other_value;
    record.difference = difference;
    record.tolerance = tolerance;
    record.active = true;
    if (conflicts_.size() >= history_limits_.max_conflicts) {
      conflicts_.erase(conflicts_.begin());
    }
    conflicts_.push_back(record);
    record_event(EventKind::ConflictDetected, Severity::Warning, &sample, state.queue,
                 "sources " + sample.source.to_string() + " and " + other_source.to_string() +
                     " disagree by " + std::to_string(difference) + " (tolerance " +
                     std::to_string(tolerance) + ")");
    result.conflict_detected = true;
  } else if (!conflict && state.conflict_active) {
    state.conflict_active = false;
    counters_.conflicts_cleared += 1u;
    for (auto it = conflicts_.rbegin(); it != conflicts_.rend(); ++it) {
      if (it->queue == state.queue && it->active) {
        it->active = false;
        break;
      }
    }
    record_event(EventKind::ConflictCleared, Severity::Notice, &sample, state.queue,
                 "peer sources agree again within tolerance");
  } else if (conflict) {
    result.conflict_detected = true;
  }
}

Status QueueStore::ingest_locked(QueueSample sample, AdmissionResult& result) {
  result.queue = sample.queue;
  counters_.samples_presented += 1u;

  // Receive time is a fact about this runtime, never a claim by the sender. The
  // canonical wire format deliberately carries no receive timestamp, so a
  // sample that arrives without one is stamped now. Without this the evidence
  // would be dated to the epoch and could never be reported as fresh.
  if (sample.received.steady.ns == 0 && sample.received.wall.ns == 0) {
    sample.received = clock_ != nullptr ? clock_->now() : receive_now();
  }

  const auto reject = [&](AdmissionOutcome outcome, ErrorCode code, std::string reason) -> Status {
    result.outcome = outcome;
    result.accepted = false;
    result.reason = reason;
    counters_.samples_rejected += 1u;
    record_event(EventKind::SampleRejected, Severity::Warning, &sample, sample.queue, reason);
    return Status::failure(code, std::move(reason));
  };
  const auto fence = [&](AdmissionOutcome outcome) -> Status {
    result.outcome = outcome;
    result.accepted = false;
    result.reason = std::string(fence_reason(outcome));
    counters_.samples_fenced += 1u;
    if (auto it = sources_.find(sample.source); it != sources_.end()) {
      it->second.fenced += 1u;
    }
    const EventKind kind = outcome == AdmissionOutcome::FencedStaleSequence
                               ? EventKind::DuplicateReplay
                               : EventKind::SampleFenced;
    record_event(kind, Severity::Warning, &sample, sample.queue, result.reason);
    return Status::success();
  };

  if (!sample.queue.valid() || !sample.source.valid() || !sample.incarnation.valid() ||
      !sample.generation.valid() || !sample.sequence.valid()) {
    return reject(AdmissionOutcome::RejectedInvalid, ErrorCode::InvalidArgument,
                  "sample identity is incomplete: queue, source, incarnation, generation and "
                  "sequence are all required");
  }
  if (sample.reported == 0u) {
    return reject(AdmissionOutcome::RejectedInvalid, ErrorCode::InvalidArgument,
                  "sample reports no measurement fields");
  }
  const FieldMask undeclared = sample.reported & ~sample.declared;
  if (sample.declared != 0u && undeclared != 0u) {
    return reject(AdmissionOutcome::RejectedInvalid, ErrorCode::InvalidArgument,
                  "sample reports fields it did not declare: " + describe_missing(undeclared));
  }

  AdmissionOutcome outcome = AdmissionOutcome::Accepted;

  // ---- source registry and fencing -------------------------------------
  auto source_it = sources_.find(sample.source);
  if (source_it == sources_.end()) {
    if (sources_.size() >= history_limits_.max_sources) {
      return reject(AdmissionOutcome::RejectedLimit, ErrorCode::LimitExceeded,
                    "the source registry is full; no further sources may be registered");
    }
    SourceRecord record;
    record.id = sample.source;
    record.authority = sample.authority;
    record.declared_authority = sample.authority;
    record.incarnation = sample.incarnation;
    record.incarnation_ordinal = next_incarnation_ordinal_++;
    record.generation = sample.generation;
    record.clock_domain = sample.observed.domain;
    record.first_seen = sample.received;
    record.last_seen = sample.received;
    record.last_observed_ns = sample.observed.ns;
    record.recent_incarnations.emplace_back(sample.incarnation, record.incarnation_ordinal);
    sources_.emplace(sample.source, std::move(record));
    counters_.sources_registered += 1u;
    outcome = AdmissionOutcome::AcceptedNewSource;
    record_event(EventKind::SourceRegistered, Severity::Notice, &sample, sample.queue,
                 "registered source with authority " +
                     std::string(to_string(sample.authority)));
    source_it = sources_.find(sample.source);
  } else {
    SourceRecord& record = source_it->second;

    // Incarnation fencing. An incarnation token is opaque; ordering comes from
    // the ordinal this runtime assigned the first time it saw the token.
    if (record.incarnation != sample.incarnation) {
      IncarnationOrdinal known_ordinal = 0;
      bool known = false;
      for (const auto& entry : record.recent_incarnations) {
        if (entry.first == sample.incarnation) {
          known_ordinal = entry.second;
          known = true;
          break;
        }
      }
      if (known && known_ordinal < record.incarnation_ordinal) {
        return fence(AdmissionOutcome::FencedStaleIncarnation);
      }
      if (known && known_ordinal == record.incarnation_ordinal) {
        return fence(AdmissionOutcome::FencedStaleIncarnation);
      }
      const IncarnationOrdinal ordinal = next_incarnation_ordinal_++;
      record.incarnation = sample.incarnation;
      record.incarnation_ordinal = ordinal;
      record.last_sequence = SourceSequence{};
      record.recent_incarnations.insert(record.recent_incarnations.begin(),
                                        {sample.incarnation, ordinal});
      while (record.recent_incarnations.size() > kMaxRecentIncarnations) {
        record.recent_incarnations.pop_back();
      }
      outcome = AdmissionOutcome::AcceptedNewIncarnation;
      record_event(EventKind::SourceIncarnationChanged, Severity::Notice, &sample, sample.queue,
                   "source presented a new incarnation token; its sequence space was reset");
    }

    // Generation fencing.
    if (sample.generation < record.generation) {
      return fence(AdmissionOutcome::FencedStaleGeneration);
    }
    if (sample.generation > record.generation) {
      record.generation = sample.generation;
      if (outcome == AdmissionOutcome::Accepted) {
        outcome = AdmissionOutcome::AcceptedNewGeneration;
      }
      record_event(EventKind::SampleAccepted, Severity::Notice, &sample, sample.queue,
                   "source advanced to generation " + sample.generation.to_string());
    }

    // Authority fencing: a source may raise its authority, never lower it.
    if (sample.authority < record.authority) {
      return fence(AdmissionOutcome::FencedAuthorityDowngrade);
    }
    if (sample.authority > record.authority) {
      record.authority = sample.authority;
      record.declared_authority = sample.authority;
      record_event(EventKind::SourceAuthorityChanged, Severity::Notice, &sample, sample.queue,
                   "source raised its authority to " +
                       std::string(to_string(sample.authority)));
    }

    // Sequence fencing: within one incarnation the sequence must be strictly
    // increasing.
    if (sample.sequence <= record.last_sequence) {
      record.duplicates += 1u;
      counters_.duplicates_suppressed += 1u;
      return fence(AdmissionOutcome::FencedStaleSequence);
    }
  }

  SourceRecord& source = source_it->second;
  source.last_sequence = sample.sequence;
  source.last_seen = sample.received;
  source.last_observed_ns = sample.observed.ns;
  if (sample.observed.known()) {
    source.clock_domain = sample.observed.domain;
  }

  // ---- queue bookkeeping ----------------------------------------------
  if (queues_.size() >= history_limits_.max_queues && queues_.find(sample.queue) == queues_.end()) {
    return reject(AdmissionOutcome::RejectedLimit, ErrorCode::LimitExceeded,
                  "the queue registry is full; no further queues may be tracked");
  }
  const std::uint64_t per_queue =
      static_cast<std::uint64_t>(history_limits_.max_samples_per_queue) *
      static_cast<std::uint64_t>(sizeof(HistoryRecord));
  if (queues_.find(sample.queue) == queues_.end()) {
    const auto needed = checked::add_u64(history_bytes_, per_queue);
    if (!needed.has_value() || *needed > history_limits_.max_history_bytes) {
      return reject(AdmissionOutcome::RejectedLimit, ErrorCode::LimitExceeded,
                    "the history byte budget is exhausted; no further queue history may be "
                    "allocated");
    }
  }

  bool created = false;
  QueueState& state = acquire_queue(sample.queue, created);
  result.queue_created = created;
  if (created) {
    state.latest.state = PressureState::Unknown;
    state.latest.queue_text = sample.queue.to_string();
    record_event(EventKind::QueueLimitReached, Severity::Debug, &sample, sample.queue,
                 "queue state created");
  }

  const MetadataResolution resolution =
      has_metadata_ ? resolve_class_attributes(metadata_, sample.queue, sample.classes)
                    : MetadataResolution{};
  if (has_metadata_ && resolution.used_metadata) {
    state.classes = resolution.attributes;
    state.classes_known = true;
  } else if (sample.classes.traffic_class.has_value() ||
             sample.classes.scheduling_class.has_value()) {
    state.classes = sample.classes;
    state.classes_known = true;
  }
  if (has_metadata_ && resolution.referenced_unknown_class &&
      !state.class_reference_unsupported) {
    state.class_reference_unsupported = true;
    record_event(EventKind::SampleAccepted, Severity::Notice, &sample, sample.queue,
                 "the sample names a class that the current metadata table does not describe");
  }

  const bool metadata_derived = has_metadata_ && resolution.used_metadata;
  const EvidenceAssessment assessment = assess(sample, source, metadata_derived);

  // ---- counter continuity ---------------------------------------------
  FieldMask counters_reported = 0;
  for (std::size_t index = 0; index < kSampleFieldCount; ++index) {
    const auto field = static_cast<SampleField>(index);
    if (is_counter_field(field) && sample.has(field)) {
      counters_reported = counters_reported | field_bit(field);
    }
  }
  state.counters_seen = state.counters_seen | counters_reported;

  std::array<CounterDelta, kSampleFieldCount> deltas{};
  bool any_unknown_delta = false;
  for (std::size_t index = 0; index < kSampleFieldCount; ++index) {
    const auto field = static_cast<SampleField>(index);
    if (!is_counter_field(field) || !has_field(state.counters_seen, field)) {
      continue;
    }
    if (!sample.has(field)) {
      any_unknown_delta = true;
      continue;
    }
    CounterUpdate update = update_counter(state.counters[index], sample.raw_value_or_zero(field),
                                          sample.observed, sample.received, policy_.continuity);
    deltas[index].continuity = update.continuity;
    deltas[index].known = update.delta_known;
    deltas[index].delta = update.delta;
    if (!update.delta_known) {
      any_unknown_delta = true;
    }
    switch (update.continuity) {
      case CounterContinuity::Wrapped:
        counters_.counter_wraps += 1u;
        record_event(EventKind::CounterWrap, Severity::Warning, &sample, sample.queue,
                     std::string(to_string(field)) + ": " + update.reason);
        break;
      case CounterContinuity::Reset:
        counters_.counter_resets += 1u;
        record_event(EventKind::CounterReset, Severity::Warning, &sample, sample.queue,
                     std::string(to_string(field)) + ": " + update.reason);
        break;
      case CounterContinuity::AmbiguousWrap:
        counters_.counter_ambiguous += 1u;
        record_event(EventKind::CounterAmbiguous, Severity::Warning, &sample, sample.queue,
                     std::string(to_string(field)) + ": " + update.reason);
        break;
      case CounterContinuity::Discontinuous:
        counters_.counter_discontinuous += 1u;
        record_event(EventKind::CounterDiscontinuous, Severity::Warning, &sample, sample.queue,
                     std::string(to_string(field)) + ": " + update.reason);
        break;
      case CounterContinuity::FirstObservation:
      case CounterContinuity::Continuous:
      case CounterContinuity::Unchanged:
      case CounterContinuity::Undetermined:
        break;
    }
  }

  // ---- conflict detection ---------------------------------------------
  detect_conflict(state, sample, source, result);

  // ---- classification --------------------------------------------------
  WindowSums sums =
      window_sums(state.history, sample.received.steady.ns - policy_.evaluation_window_ns);
  accumulate_delta(sums.drops, deltas[static_cast<std::size_t>(SampleField::DropPackets)]);
  accumulate_delta(sums.marks, deltas[static_cast<std::size_t>(SampleField::MarkPackets)]);
  accumulate_delta(sums.pause_frames,
                   deltas[static_cast<std::size_t>(SampleField::PauseFrames)]);
  accumulate_delta(sums.pause_duration,
                   deltas[static_cast<std::size_t>(SampleField::PauseDurationNanos)]);
  accumulate_delta(sums.enqueues, deltas[static_cast<std::size_t>(SampleField::EnqueuePackets)]);
  accumulate_delta(sums.dequeues,
                   deltas[static_cast<std::size_t>(SampleField::DequeuePackets)]);

  ClassificationInput input = make_classification_input(sample);
  input.assessment = assessment;
  input.peer_conflict = state.conflict_active;
  input.drops = sums.drops;
  input.marks = sums.marks;
  input.pause_frames = sums.pause_frames;
  input.pause_duration = sums.pause_duration;
  input.enqueues = sums.enqueues;
  input.dequeues = sums.dequeues;

  ClassificationResult classification = classify(policy_, input);
  const PressureState previous_state = state.latest.state;
  const bool had_state = !state.history.empty();
  result.previous_state = previous_state;
  result.state_changed = had_state && previous_state != classification.state;
  if (result.state_changed) {
    counters_.state_transitions += 1u;
    record_event(EventKind::StateTransition, Severity::Notice, &sample, sample.queue,
                 std::string(to_string(previous_state)) + " -> " +
                     std::string(to_string(classification.state)) + " by " +
                     std::string(rule_name(classification.deciding_rule)));
  }
  state.latest = classification;
  state.last_input = input;

  // ---- history ---------------------------------------------------------
  HistoryRecord record;
  record.received = sample.received;
  record.observed = sample.observed;
  record.source = sample.source;
  record.generation = sample.generation;
  record.incarnation_ordinal = source.incarnation_ordinal;
  record.sequence = sample.sequence;
  record.authority = source.authority;
  record.reported = sample.reported;
  record.freshness = assessment.freshness;
  record.quality = assessment.quality;
  record.flags = assessment.flags;
  record.age_ns = assessment.age_ns;
  for (std::size_t index = 0; index < kSampleFieldCount; ++index) {
    record.values[index] = sample.values_at(index);
    record.deltas[index] = deltas[index];
  }
  record.state = classification.state;
  record.state_digest = classification.explanation_digest;
  const std::uint64_t evicted_before = state.history.evicted();
  state.history.push(record);
  state.last_received = sample.received;
  state.last_observed = sample.observed;
  state.accepted += 1u;
  counters_.history_records_pushed += 1u;
  const std::uint64_t evicted_after = state.history.evicted();
  if (evicted_after > evicted_before) {
    const std::uint64_t newly_evicted = evicted_after - evicted_before;
    counters_.history_records_evicted += newly_evicted;
    record_event(EventKind::HistoryEvicted, Severity::Debug, &sample, sample.queue,
                 "evicted " + std::to_string(newly_evicted) +
                     " history records to stay inside the ring capacity");
  }

  // ---- peer table ------------------------------------------------------
  const auto occupancy = sample.occupancy_for_pressure();
  PeerObservation peer;
  peer.source = sample.source;
  peer.authority = source.authority;
  peer.reported = sample.reported;
  peer.received_ns = sample.received.steady.ns;
  peer.observed = sample.observed;
  if (occupancy.has_value()) {
    peer.has_occupancy = true;
    peer.occupancy = *occupancy;
    peer.occupancy_field = sample.has(SampleField::OccupancyCells)
                               ? SampleField::OccupancyCells
                               : SampleField::OccupancyBytes;
  }
  bool peer_updated = false;
  for (PeerObservation& existing : state.peers) {
    if (existing.source == sample.source) {
      existing = peer;
      peer_updated = true;
      break;
    }
  }
  if (!peer_updated) {
    if (state.peers.size() >= history_limits_.max_sources) {
      state.peers.erase(state.peers.begin());
    }
    state.peers.push_back(peer);
  }

  // ---- totals ----------------------------------------------------------
  source.accepted += 1u;
  counters_.samples_accepted += 1u;
  result.outcome = outcome;
  result.accepted = true;
  result.classification = std::move(classification);
  return Status::success();
}

Status QueueStore::ingest(const QueueSample& sample, AdmissionResult& result) {
  if (!config_status_.ok()) {
    return config_status_;
  }
  std::unique_lock<std::shared_mutex> lock(mutex_);
  LockRankGuard guard(kRankStore, "store");
  return ingest_locked(sample, result);
}

Status QueueStore::ingest_batch(const std::vector<QueueSample>& samples, const StopToken& token,
                                BatchAdmission& result) {
  result = BatchAdmission{};
  if (!config_status_.ok()) {
    return config_status_;
  }
  std::unique_lock<std::shared_mutex> lock(mutex_);
  LockRankGuard guard(kRankStore, "store");
  for (std::size_t index = 0; index < samples.size(); ++index) {
    if (token.stop_requested()) {
      return Status::failure(ErrorCode::Cancelled,
                             "batch ingest was cancelled after " + std::to_string(index) +
                                 " of " + std::to_string(samples.size()) + " samples");
    }
    AdmissionResult admission;
    const Status status = ingest_locked(samples[index], admission);
    result.presented += 1u;
    if (!status.ok()) {
      result.rejected += 1u;
      if (result.failures.size() < kMaxFailureDetails) {
        AdmissionFailure failure;
        failure.index = index;
        failure.outcome = admission.outcome;
        failure.queue = admission.queue;
        failure.reason = admission.reason.empty() ? status.message() : admission.reason;
        result.failures.push_back(std::move(failure));
      } else {
        result.failures_truncated = true;
      }
      continue;
    }
    switch (admission.outcome) {
      case AdmissionOutcome::Accepted:
      case AdmissionOutcome::AcceptedNewSource:
      case AdmissionOutcome::AcceptedNewIncarnation:
      case AdmissionOutcome::AcceptedNewGeneration:
      case AdmissionOutcome::AcceptedLateButFresh:
        result.accepted += 1u;
        break;
      case AdmissionOutcome::FencedStaleSequence:
        result.duplicates += 1u;
        result.fenced += 1u;
        break;
      default:
        result.fenced += 1u;
        break;
    }
    if (admission.conflict_detected) {
      result.conflicts += 1u;
    }
    if (admission.state_changed) {
      result.state_changes += 1u;
    }
    if (admission.queue_created) {
      result.queues_created += 1u;
    }
  }
  return Status::success();
}

Status QueueStore::apply_metadata(ClassMetadataTable table, MetadataAdmission& result) {
  result = MetadataAdmission{};
  if (!config_status_.ok()) {
    return config_status_;
  }
  std::unique_lock<std::shared_mutex> lock(mutex_);
  LockRankGuard guard(kRankStore, "store");

  const Status valid = validate_metadata(table, history_limits_.max_queues);
  if (!valid.ok()) {
    return valid;
  }
  if (has_metadata_) {
    const bool newer_generation = table.generation > metadata_.generation;
    const bool same_generation_newer_revision =
        table.generation == metadata_.generation && table.revision > metadata_.revision;
    if (!newer_generation && !same_generation_newer_revision) {
      result.fenced = true;
      result.previous_revision = metadata_.revision;
      result.reason =
          "metadata revision is not newer than the stored revision for this generation";
      counters_.metadata_fenced += 1u;
      record_event_global(EventKind::MetadataRevisionFenced, Severity::Warning,
                          result.reason + " (revision " + table.revision.to_string() +
                              ", source " + table.source.to_string() + ")");
      return Status::success();
    }
    result.previous_revision = metadata_.revision;
  }
  metadata_ = std::move(table);
  has_metadata_ = true;
  counters_.metadata_updates += 1u;
  result.accepted = true;
  record_event_global(EventKind::MetadataUpdated, Severity::Notice,
                      "metadata revision " + metadata_.revision.to_string() + " for generation " +
                          metadata_.generation.to_string() + " accepted");
  return Status::success();
}

std::string_view to_string(ExportFormat format) noexcept {
  switch (format) {
    case ExportFormat::Ndjson:
      return "ndjson";
    case ExportFormat::Text:
      return "text";
  }
  return "ndjson";
}

std::optional<ExportFormat> parse_export_format(std::string_view text) noexcept {
  if (text == "ndjson") {
    return ExportFormat::Ndjson;
  }
  if (text == "text") {
    return ExportFormat::Text;
  }
  return std::nullopt;
}

bool matches_filter(const QueueFilter& filter, const QueuePath& queue,
                    const QueueClassAttributes& classes, PressureState state) {
  if (filter.device.has_value() && queue.device != *filter.device) {
    return false;
  }
  if (filter.port.has_value() && queue.port != *filter.port) {
    return false;
  }
  if (filter.queue.has_value() && queue.queue != *filter.queue) {
    return false;
  }
  if (filter.traffic_class.has_value() &&
      (!classes.traffic_class.has_value() || *classes.traffic_class != *filter.traffic_class)) {
    return false;
  }
  if (filter.scheduling_class.has_value() && (!classes.scheduling_class.has_value() ||
                                              *classes.scheduling_class != *filter.scheduling_class)) {
    return false;
  }
  if (filter.state.has_value() && state != *filter.state) {
    return false;
  }
  return true;
}

void QueueStore::build_inspect_row(const QueueState& state, InspectRow& row) const {
  row.queue = state.queue;
  row.classes = state.classes;
  row.state = state.latest.state;
  row.assessment = state.latest.assessment;
  row.explanation_summary = render_explanation_summary(state.latest);
  row.last_received = state.last_received;
  row.last_observed = state.last_observed;
  row.history_records = state.history.size();
  row.history_capacity = state.history.capacity();
  row.history_evicted = state.history.evicted();
  row.accepted = state.accepted;
  row.conflicts = state.conflicts;
  row.conflict_active = state.conflict_active;
  row.class_reference_unsupported = state.class_reference_unsupported;
  for (std::size_t index = 0; index < kSampleFieldCount; ++index) {
    const CounterState& counter = state.counters[index];
    row.counter_wraps += counter.wraps;
    row.counter_resets += counter.resets;
    row.counter_ambiguous += counter.ambiguous;
    row.counter_discontinuous += counter.discontinuities;
  }
}

void QueueStore::build_pressure_row(const QueueState& state, bool include_trace,
                                    PressureRow& row) const {
  row.queue = state.queue;
  row.classes = state.classes;
  row.state = state.latest.state;
  row.assessment = state.latest.assessment;
  row.basis = state.latest.basis;
  row.deciding_rule = state.latest.deciding_rule;
  row.explanation_digest = state.latest.explanation_digest;
  if (include_trace) {
    row.trace = state.latest.trace;
  }
}

Status QueueStore::inspect(const InspectQuery& query, InspectResult& result) const {
  result = InspectResult{};
  if (!config_status_.ok()) {
    return config_status_;
  }
  std::shared_lock<std::shared_mutex> lock(mutex_);
  LockRankGuard guard(kRankStore, "store");
  const std::size_t limit =
      query.page.limit == 0u ? query_limits_.default_rows
                             : std::min(query.page.limit, query_limits_.max_rows);
  for (const auto& entry : queues_) {
    const QueueState& state = entry.second;
    if (!matches_filter(query.filter, entry.first, state.classes, state.latest.state)) {
      continue;
    }
    ++result.total_matched;
    if (result.total_matched <= query.page.offset) {
      continue;
    }
    if (result.rows.size() >= limit) {
      result.truncated = true;
      continue;
    }
    InspectRow row;
    build_inspect_row(state, row);
    result.rows.push_back(std::move(row));
  }
  return Status::success();
}

Status QueueStore::history(const HistoryQuery& query, HistoryResult& result) const {
  result = HistoryResult{};
  result.queue = query.queue;
  if (!config_status_.ok()) {
    return config_status_;
  }
  if (!query.queue.valid()) {
    return Status::failure(ErrorCode::InvalidArgument, "a history query requires a queue path");
  }
  const Status spec_status = validate_window_spec(query.window);
  if (!spec_status.ok()) {
    return spec_status;
  }

  std::shared_lock<std::shared_mutex> lock(mutex_);
  LockRankGuard guard(kRankStore, "store");
  const auto found = queues_.find(query.queue);
  if (found == queues_.end()) {
    result.found = false;
    return Status::success();
  }
  const QueueState& state = found->second;
  const Nanos now_ns = clock_ != nullptr ? clock_->now().steady.ns : receive_now().steady.ns;

  BucketAccumulator accumulator;
  QOBS_TRY(accumulator.configure(query.window));
  for (std::size_t index = 0; index < state.history.size(); ++index) {
    accumulator.observe(to_observation(state.history.at(index)));
  }
  accumulator.expire(now_ns);
  result.aggregate = accumulator.aggregate(now_ns, policy_.freshness);
  result.found = true;
  result.records_retained = state.history.size();
  result.records_total = state.history.size();
  result.records_evicted = state.history.evicted();

  if (query.include_records) {
    const std::size_t limit =
        query.page.limit == 0u ? query_limits_.default_rows
                               : std::min(query.page.limit, query_limits_.max_rows);
    std::size_t matched = 0;
    for (std::size_t index = 0; index < state.history.size(); ++index) {
      ++matched;
      if (matched <= query.page.offset) {
        continue;
      }
      if (result.records.size() >= limit) {
        result.truncated = true;
        continue;
      }
      result.records.push_back(state.history.at(index));
    }
  }
  return Status::success();
}

Status QueueStore::pressure(const PressureQuery& query, PressureResult& result) const {
  result = PressureResult{};
  if (!config_status_.ok()) {
    return config_status_;
  }
  std::shared_lock<std::shared_mutex> lock(mutex_);
  LockRankGuard guard(kRankStore, "store");
  const std::size_t limit =
      query.page.limit == 0u ? query_limits_.default_rows
                             : std::min(query.page.limit, query_limits_.max_rows);
  for (const auto& entry : queues_) {
    const QueueState& state = entry.second;
    if (!matches_filter(query.filter, entry.first, state.classes, state.latest.state)) {
      continue;
    }
    ++result.total_matched;
    if (result.total_matched <= query.page.offset) {
      continue;
    }
    if (result.rows.size() >= limit) {
      result.truncated = true;
      continue;
    }
    PressureRow row;
    build_pressure_row(state, query.include_trace, row);
    result.rows.push_back(std::move(row));
  }
  return Status::success();
}

Status QueueStore::explain(const ExplainQuery& query, ExplainResult& result) const {
  result = ExplainResult{};
  result.queue = query.queue;
  if (!config_status_.ok()) {
    return config_status_;
  }
  if (!query.queue.valid()) {
    return Status::failure(ErrorCode::InvalidArgument, "an explain query requires a queue path");
  }
  if (query.policy.has_value()) {
    const Status policy_status = validate_policy(*query.policy);
    if (!policy_status.ok()) {
      return policy_status;
    }
  }

  std::shared_lock<std::shared_mutex> lock(mutex_);
  LockRankGuard guard(kRankStore, "store");
  const auto found = queues_.find(query.queue);
  if (found == queues_.end()) {
    result.found = false;
    return Status::success();
  }
  const QueueState& state = found->second;
  result.found = true;
  if (query.policy.has_value()) {
    result.policy_overridden = true;
    result.classification = classify(*query.policy, state.last_input);
  } else {
    result.classification = state.latest;
  }
  result.lines = explanation_lines(result.classification);
  result.explanation = render_explanation(result.classification);
  return Status::success();
}

Status QueueStore::events(const EventQuery& query, EventResult& result) const {
  result = EventResult{};
  if (!config_status_.ok()) {
    return config_status_;
  }
  std::shared_lock<std::shared_mutex> lock(mutex_);
  LockRankGuard guard(kRankStore, "store");
  const std::size_t limit =
      query.page.limit == 0u ? query_limits_.default_rows
                             : std::min(query.page.limit, query_limits_.max_rows);
  for (std::size_t offset = 0; offset < event_size_; ++offset) {
    const EventRecord& record = event_log_[(event_head_ + offset) % event_log_.size()];
    if (query.kind.has_value() && record.kind != *query.kind) {
      continue;
    }
    if (query.queue.has_value() && !(record.queue == *query.queue)) {
      continue;
    }
    if (query.min_severity.has_value() && record.severity < *query.min_severity) {
      continue;
    }
    ++result.total_matched;
    if (result.total_matched <= query.page.offset) {
      continue;
    }
    if (result.events.size() >= limit) {
      result.truncated = true;
      continue;
    }
    result.events.push_back(record);
  }
  return Status::success();
}

Status QueueStore::sources(std::vector<SourceRecord>& result) const {
  result.clear();
  if (!config_status_.ok()) {
    return config_status_;
  }
  std::shared_lock<std::shared_mutex> lock(mutex_);
  LockRankGuard guard(kRankStore, "store");
  result.reserve(sources_.size());
  for (const auto& entry : sources_) {
    result.push_back(entry.second);
  }
  return Status::success();
}

Status QueueStore::conflicts(std::vector<ConflictRecord>& result) const {
  result.clear();
  if (!config_status_.ok()) {
    return config_status_;
  }
  std::shared_lock<std::shared_mutex> lock(mutex_);
  LockRankGuard guard(kRankStore, "store");
  result = conflicts_;
  return Status::success();
}

bool QueueStore::is_contending_state(PressureState state) const {
  switch (state) {
    case PressureState::Pressured:
      return policy_.contention.count_pressured;
    case PressureState::Saturated:
      return policy_.contention.count_saturated;
    case PressureState::Dropping:
      return policy_.contention.count_dropping;
    case PressureState::Paused:
      return policy_.contention.count_paused;
    default:
      return false;
  }
}

Status QueueStore::microburst(const MicroburstQuery& query, MicroburstResult& result) const {
  result = MicroburstResult{};
  if (!config_status_.ok()) {
    return config_status_;
  }
  std::shared_lock<std::shared_mutex> lock(mutex_);
  LockRankGuard guard(kRankStore, "store");
  const std::size_t limit =
      query.page.limit == 0u ? query_limits_.default_rows
                             : std::min(query.page.limit, query_limits_.max_rows);
  for (const auto& entry : queues_) {
    const QueueState& state = entry.second;
    if (!matches_filter(query.filter, entry.first, state.classes, state.latest.state)) {
      continue;
    }
    ++result.queues_examined;
    MicroburstRecord record;
    build_microburst(state, record);
    if (!record.coverage_sufficient) {
      ++result.queues_insufficient;
      if (!query.include_insufficient) {
        continue;
      }
    }
    ++result.total_matched;
    if (result.total_matched <= query.page.offset) {
      continue;
    }
    if (result.records.size() >= limit) {
      result.truncated = true;
      continue;
    }
    result.records.push_back(std::move(record));
  }
  return Status::success();
}

void QueueStore::build_microburst(const QueueState& state, MicroburstRecord& record) const {
  record = MicroburstRecord{};
  record.queue = state.queue;
  record.window = microburst_spec_;
  BucketAccumulator accumulator;
  if (!accumulator.configure(microburst_spec_).ok()) {
    return;
  }
  for (std::size_t index = 0; index < state.history.size(); ++index) {
    accumulator.observe(to_observation(state.history.at(index)));
  }
  const Nanos now_ns = clock_ != nullptr ? clock_->now().steady.ns : receive_now().steady.ns;
  accumulator.expire(now_ns);
  const auto evidence = accumulator.burst_evidence(policy_.microburst.min_excursion,
                                                   policy_.microburst.min_covered_buckets);
  record.peak = evidence.peak;
  record.trough = evidence.trough;
  record.excursion = evidence.excursion;
  record.required_excursion = evidence.required_excursion;
  record.covered_buckets = evidence.covered_buckets;
  record.required_buckets = evidence.required_buckets;
  record.peak_at_ns = evidence.peak_at_ns;
  record.trough_at_ns = evidence.trough_at_ns;
  record.detected = evidence.detected;
  record.coverage_sufficient = evidence.coverage_sufficient;
  record.sub_sample_reconstructed = false;
  record.assessment = accumulator.aggregate(now_ns, policy_.freshness).assessment;
  if (!record.coverage_sufficient) {
    record.assessment.quality = EvidenceQuality::Incomplete;
    record.assessment.flags = with_flag(record.assessment.flags, EvidenceFlag::CoverageInsufficient);
    record.assessment.reason =
        "only " + std::to_string(record.covered_buckets) + " of the required " +
        std::to_string(record.required_buckets) +
        " buckets contain observations; no burst conclusion is drawn";
  }
}

Status QueueStore::contention(const ContentionQuery& query, ContentionResult& result) const {
  result = ContentionResult{};
  if (!config_status_.ok()) {
    return config_status_;
  }
  std::shared_lock<std::shared_mutex> lock(mutex_);
  LockRankGuard guard(kRankStore, "store");

  using BucketFlags = std::map<Nanos, std::pair<bool, bool>>;  // covered, contending
  std::map<PortPath, std::map<SchedulingClassId, std::vector<std::pair<QueuePath, BucketFlags>>>>
      groups;

  const Nanos now_ns = clock_ != nullptr ? clock_->now().steady.ns : receive_now().steady.ns;
  const Nanos cutoff = now_ns - contention_spec_.duration_ns;

  for (const auto& entry : queues_) {
    const QueueState& state = entry.second;
    if (!state.classes_known || !state.classes.scheduling_class.has_value()) {
      continue;
    }
    PortPath port;
    port.device = entry.first.device;
    port.port = entry.first.port;
    if (query.port.has_value() && !(port == *query.port)) {
      continue;
    }
    if (query.scheduling_class.has_value() &&
        *state.classes.scheduling_class != *query.scheduling_class) {
      continue;
    }
    BucketFlags flags;
    for (std::size_t index = 0; index < state.history.size(); ++index) {
      const HistoryRecord& record = state.history.at(index);
      if (record.received.steady.ns < cutoff) {
        continue;
      }
      const Nanos start = align_bucket(record.received.steady.ns, contention_spec_.bucket_ns);
      auto& slot = flags[start];
      slot.first = true;
      if (is_contending_state(record.state)) {
        slot.second = true;
      }
    }
    groups[port][*state.classes.scheduling_class].emplace_back(entry.first, std::move(flags));
  }

  const std::size_t limit =
      query.page.limit == 0u ? query_limits_.default_rows
                             : std::min(query.page.limit, query_limits_.max_rows);
  for (auto& port_entry : groups) {
    ++result.ports_examined;
    for (auto& class_entry : port_entry.second) {
      auto& siblings = class_entry.second;
      for (std::size_t left = 0; left < siblings.size(); ++left) {
        for (std::size_t right = left + 1u; right < siblings.size(); ++right) {
          ++result.pairs_examined;
          std::size_t union_buckets = 0;
          std::size_t simultaneous = 0;
          for (const auto& bucket : siblings[left].second) {
            const auto other = siblings[right].second.find(bucket.first);
            const bool covered_other = other != siblings[right].second.end();
            if (!bucket.second.first && !covered_other) {
              continue;
            }
            ++union_buckets;
            if (bucket.second.second && covered_other && other->second.second) {
              ++simultaneous;
            }
          }
          if (union_buckets < policy_.contention.min_covered_buckets) {
            ++result.pairs_insufficient_coverage;
            continue;
          }
          if (simultaneous < policy_.contention.min_simultaneous_buckets) {
            continue;
          }
          ContentionRecord record;
          record.port = port_entry.first;
          record.scheduling_class = class_entry.first;
          record.queue_a = siblings[left].first;
          record.queue_b = siblings[right].first;
          record.window = contention_spec_;
          record.covered_buckets = union_buckets;
          record.simultaneous_buckets = simultaneous;
          record.union_buckets = union_buckets;
          record.ratio_numerator = simultaneous;
          record.ratio_denominator = union_buckets;
          reduce_fraction(record.ratio_numerator, record.ratio_denominator);
          record.window_end_ns = now_ns;
          record.window_start_ns = cutoff;
          record.assessment.provenance = Provenance::Correlated;
          record.assessment.quality = EvidenceQuality::Complete;
          record.assessment.freshness = assess_freshness(0, policy_.freshness);
          record.assessment.authority = SourceAuthority::Primary;
          record.assessment.reason =
              "both queues were observed under pressure in " + std::to_string(simultaneous) +
              " of " + std::to_string(union_buckets) + " covered buckets";
          ++result.total_matched;
          if (result.total_matched <= query.page.offset) {
            continue;
          }
          if (result.records.size() >= limit) {
            result.truncated = true;
            continue;
          }
          result.records.push_back(std::move(record));
        }
      }
    }
  }
  return Status::success();
}

Status QueueStore::export_data(const ExportQuery& query, ExportResult& result) const {
  result = ExportResult{};
  if (!config_status_.ok()) {
    return config_status_;
  }
  if (query.include_history) {
    const Status spec_status = validate_window_spec(query.window);
    if (!spec_status.ok()) {
      return spec_status;
    }
  }
  std::shared_lock<std::shared_mutex> lock(mutex_);
  LockRankGuard guard(kRankStore, "store");
  const std::size_t limit =
      query.page.limit == 0u ? query_limits_.default_rows
                             : std::min(query.page.limit, query_limits_.max_rows);
  const Nanos now_ns = clock_ != nullptr ? clock_->now().steady.ns : receive_now().steady.ns;

  JsonWriter writer;
  std::string text_out;
  std::size_t matched = 0;
  for (const auto& entry : queues_) {
    const QueueState& state = entry.second;
    if (!matches_filter(query.filter, entry.first, state.classes, state.latest.state)) {
      continue;
    }
    ++matched;
    if (matched <= query.page.offset) {
      continue;
    }
    if (result.rows >= limit) {
      result.truncated = true;
      continue;
    }

    const bool json = query.format == ExportFormat::Ndjson;
    if (json) {
      JsonWriter line;
      QOBS_TRY(line.begin_object());
      QOBS_TRY(line.member_string("record", "queue_state"));
      QOBS_TRY(line.member_u64("wire_version", QOBS_WIRE_FORMAT_VERSION));
      QOBS_TRY(line.member_string("runtime_id", session_.runtime_id));
      QOBS_TRY(line.member_string("policy", state.latest.policy_name));
      QOBS_TRY(line.member_u64("policy_version", state.latest.policy_version));
      QOBS_TRY(line.member_string("queue", entry.first.to_string()));
      QOBS_TRY(line.member_string("state", to_string(state.latest.state)));
      QOBS_TRY(line.member_string("assessment", describe(state.latest.assessment)));
      QOBS_TRY(line.member_string("basis", state.latest.basis.label));
      QOBS_TRY(line.member_string("deciding_rule", rule_name(state.latest.deciding_rule)));
      QOBS_TRY(line.member_u64("explanation_digest", state.latest.explanation_digest));
      QOBS_TRY(line.member_u64("history_records",
                               static_cast<std::uint64_t>(state.history.size())));
      QOBS_TRY(line.member_u64("history_capacity",
                               static_cast<std::uint64_t>(state.history.capacity())));
      QOBS_TRY(line.member_u64("accepted", state.accepted));
      QOBS_TRY(line.member_u64("conflicts", state.conflicts));
      QOBS_TRY(line.member_string("last_received_wall",
                                  format_wall_utc(state.last_received.wall)));
      QOBS_TRY(
          line.member_string("last_received_steady", format_steady(state.last_received.steady)));
      QOBS_TRY(line.end_object());
      result.document.append(line.buffer());
      result.document.push_back('\n');
    } else {
      text_out.append(entry.first.to_string());
      text_out.append(" state=");
      text_out.append(to_string(state.latest.state));
      text_out.append(" ");
      text_out.append(render_explanation_summary(state.latest));
      text_out.push_back('\n');
    }
    ++result.rows;

    if (query.include_history) {
      BucketAccumulator accumulator;
      QOBS_TRY(accumulator.configure(query.window));
      for (std::size_t index = 0; index < state.history.size(); ++index) {
        accumulator.observe(to_observation(state.history.at(index)));
      }
      accumulator.expire(now_ns);
      if (json) {
        for (const WindowBucket& bucket : accumulator.buckets()) {
          if (bucket.sample_count == 0u) {
            continue;
          }
          JsonWriter line;
          QOBS_TRY(line.begin_object());
          QOBS_TRY(line.member_string("record", "window_bucket"));
          QOBS_TRY(line.member_string("queue", entry.first.to_string()));
          QOBS_TRY(line.member_string("window", query.window.name));
          QOBS_TRY(line.member_i64("bucket_start_steady_ns", bucket.start_ns));
          QOBS_TRY(line.member_u64("samples", static_cast<std::uint64_t>(bucket.sample_count)));
          if (bucket.has_occupancy) {
            QOBS_TRY(line.member_u64("occupancy_max", bucket.occupancy_max));
            QOBS_TRY(line.member_u64("occupancy_min", bucket.occupancy_min));
            QOBS_TRY(line.member_u64("occupancy_last", bucket.occupancy_last));
          }
          QOBS_TRY(line.member_u64("drops", bucket.drops));
          QOBS_TRY(line.member_u64("marks", bucket.marks));
          QOBS_TRY(line.member_u64("pause_frames", bucket.pause_frames));
          QOBS_TRY(line.member_u64("pause_nanos", bucket.pause_nanos));
          QOBS_TRY(line.member_u64("unknown_delta_samples", bucket.unknown_delta_samples));
          QOBS_TRY(line.end_object());
          result.document.append(line.buffer());
          result.document.push_back('\n');
          ++result.history_records;
        }
      }
    }
  }
  if (query.format == ExportFormat::Text) {
    result.document = std::move(text_out);
  }
  return Status::success();
}

StoreCounters QueueStore::counters() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  LockRankGuard guard(kRankStore, "store");
  return counters_;
}

SessionInfo QueueStore::session() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  LockRankGuard guard(kRankStore, "store");
  return session_;
}

std::size_t QueueStore::queue_count() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  LockRankGuard guard(kRankStore, "store");
  return queues_.size();
}

std::uint64_t QueueStore::history_bytes() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  LockRankGuard guard(kRankStore, "store");
  return history_bytes_;
}

Status QueueStore::metadata_snapshot(ClassMetadataTable& result, bool& has_metadata) const {
  if (!config_status_.ok()) {
    return config_status_;
  }
  std::shared_lock<std::shared_mutex> lock(mutex_);
  LockRankGuard guard(kRankStore, "store");
  result = metadata_;
  has_metadata = has_metadata_;
  return Status::success();
}

Status QueueStore::for_each_queue(
    const std::function<void(const QueuePath&, const std::vector<HistoryRecord>&)>& visitor,
    std::size_t max_queues) const {
  if (!config_status_.ok()) {
    return config_status_;
  }
  if (!visitor) {
    return Status::failure(ErrorCode::InvalidArgument, "a visitor callback is required");
  }
  std::vector<std::pair<QueuePath, std::vector<HistoryRecord>>> extracted;
  {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    LockRankGuard guard(kRankStore, "store");
    extracted.reserve(std::min(max_queues, queues_.size()));
    for (const auto& entry : queues_) {
      if (extracted.size() >= max_queues) {
        break;
      }
      extracted.emplace_back(entry.first, entry.second.history.snapshot());
    }
  }
  // The lock is released before the visitor runs. A caller-supplied callback is
  // never invoked while a Queue Observatory lock is held.
  LockAudit::note_callback("store_for_each_queue");
  for (const auto& entry : extracted) {
    visitor(entry.first, entry.second);
  }
  return Status::success();
}

}  // namespace qobs
