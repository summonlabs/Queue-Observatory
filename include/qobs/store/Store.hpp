#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <vector>

#include "qobs/core/Cancellation.hpp"
#include "qobs/core/Limits.hpp"
#include "qobs/core/Time.hpp"
#include "qobs/model/Event.hpp"
#include "qobs/model/Metadata.hpp"
#include "qobs/model/Sample.hpp"
#include "qobs/policy/Policy.hpp"
#include "qobs/store/Correlation.hpp"
#include "qobs/store/History.hpp"
#include "qobs/store/Query.hpp"
#include "qobs/store/Window.hpp"
#include "qobs/version.hpp"

namespace qobs {

/// Why a sample was or was not admitted. Every rejection has a named cause.
enum class AdmissionOutcome : std::uint8_t {
  Accepted = 0,
  AcceptedNewSource,
  AcceptedNewIncarnation,
  AcceptedNewGeneration,
  AcceptedLateButFresh,
  RejectedInvalid,
  RejectedLimit,
  RejectedNotRunning,
  FencedStaleIncarnation,
  FencedStaleSequence,
  FencedStaleGeneration,
  FencedAuthorityDowngrade,
};

[[nodiscard]] std::string_view to_string(AdmissionOutcome outcome) noexcept;
[[nodiscard]] bool outcome_admitted(AdmissionOutcome outcome) noexcept;

/// Fence reason codes are part of the evidence trail; they are stable strings.
[[nodiscard]] std::string_view fence_reason(AdmissionOutcome outcome) noexcept;

/// What the runtime knows about one source.
struct SourceRecord {
  SourceId id{};
  SourceAuthority authority{SourceAuthority::Unknown};
  /// Declared authority observed on the wire, kept so that a downgrade can be
  /// detected even after the record was updated.
  SourceAuthority declared_authority{SourceAuthority::Unknown};
  IncarnationId incarnation{};
  IncarnationOrdinal incarnation_ordinal{0};
  SourceSequence last_sequence{};
  GenerationId generation{};
  ClockDomainId clock_domain{};
  ReceiveTime first_seen{};
  ReceiveTime last_seen{};
  std::uint64_t accepted{0};
  std::uint64_t fenced{0};
  std::uint64_t duplicates{0};
  std::uint64_t rejected{0};
  std::uint64_t last_observed_ns{0};
  /// Bounded history of recently seen incarnations, newest first.
  std::vector<std::pair<IncarnationId, IncarnationOrdinal>> recent_incarnations{};
};

/// A disagreement between two equally authoritative sources.
struct ConflictRecord {
  QueuePath queue{};
  ReceiveTime detected{};
  SourceId source_a{};
  SourceId source_b{};
  std::uint64_t value_a{0};
  std::uint64_t value_b{0};
  std::uint64_t difference{0};
  std::uint64_t tolerance{0};
  bool active{true};
};

struct AdmissionResult {
  AdmissionOutcome outcome{AdmissionOutcome::Accepted};
  bool accepted{false};
  std::string reason{};
  QueuePath queue{};
  ClassificationResult classification{};
  bool conflict_detected{false};
  bool state_changed{false};
  PressureState previous_state{PressureState::Unknown};
  bool queue_created{false};
};

struct AdmissionFailure {
  std::size_t index{0};
  AdmissionOutcome outcome{AdmissionOutcome::RejectedInvalid};
  QueuePath queue{};
  std::string reason{};
};

struct BatchAdmission {
  std::size_t presented{0};
  std::size_t accepted{0};
  std::size_t rejected{0};
  std::size_t fenced{0};
  std::size_t duplicates{0};
  std::size_t conflicts{0};
  std::size_t state_changes{0};
  std::size_t queues_created{0};
  std::vector<AdmissionFailure> failures{};
  bool failures_truncated{false};
};

struct MetadataAdmission {
  bool accepted{false};
  bool fenced{false};
  std::string reason{};
  Revision previous_revision{};
};

/// Runtime-wide counters. They are monotonic for the lifetime of the store.
struct StoreCounters {
  std::uint64_t samples_presented{0};
  std::uint64_t samples_accepted{0};
  std::uint64_t samples_rejected{0};
  std::uint64_t samples_fenced{0};
  std::uint64_t duplicates_suppressed{0};
  std::uint64_t conflicts_detected{0};
  std::uint64_t conflicts_cleared{0};
  std::uint64_t state_transitions{0};
  std::uint64_t events_recorded{0};
  std::uint64_t events_evicted{0};
  std::uint64_t history_records_pushed{0};
  std::uint64_t history_records_evicted{0};
  std::uint64_t sources_registered{0};
  std::uint64_t metadata_updates{0};
  std::uint64_t metadata_fenced{0};
  std::uint64_t counter_wraps{0};
  std::uint64_t counter_resets{0};
  std::uint64_t counter_ambiguous{0};
  std::uint64_t counter_discontinuous{0};
};

struct SessionInfo {
  std::string runtime_id{};
  ReceiveTime started{};
  std::uint32_t policy_version{0};
  std::string policy_name{};
  std::uint32_t persistence_format_version{QOBS_PERSISTENCE_FORMAT_VERSION};
  std::uint32_t wire_format_version{QOBS_WIRE_FORMAT_VERSION};
  Version runtime_version{};
};

/// The bounded, in-memory evidence store.
///
/// Concurrency contract: a single shared_mutex guards the whole store. Ingest
/// takes the exclusive lock; queries take the shared lock. No callback is ever
/// invoked while the lock is held, and the lock is always the only Queue
/// Observatory rank held by a writer (rank kRankStore).
class QueueStore {
 public:
  QueueStore(PressurePolicy policy, HistoryLimits history, QueryLimits query,
             std::shared_ptr<Clock> clock);
  ~QueueStore();

  QueueStore(const QueueStore&) = delete;
  QueueStore& operator=(const QueueStore&) = delete;
  QueueStore(QueueStore&&) = delete;
  QueueStore& operator=(QueueStore&&) = delete;

  [[nodiscard]] const PressurePolicy& policy() const noexcept { return policy_; }
  [[nodiscard]] const HistoryLimits& history_limits() const noexcept { return history_limits_; }
  [[nodiscard]] const QueryLimits& query_limits() const noexcept { return query_limits_; }

  /// Admit one observation.
  Status ingest(const QueueSample& sample, AdmissionResult& result);

  /// Admit a batch. Cancellation is honoured between samples.
  Status ingest_batch(const std::vector<QueueSample>& samples, const StopToken& token,
                      BatchAdmission& result);

  /// Admit a class metadata revision.
  Status apply_metadata(ClassMetadataTable table, MetadataAdmission& result);

  /// Re-admit evidence that was read back from persistence.
  ///
  /// Every admitted sample is marked with the recovered provenance and its
  /// freshness is capped so that it can never be reported as fresh. Recovered
  /// history is therefore explainable but never establishes current pressure.
  Status admit_recovered(const std::vector<QueueSample>& samples, BatchAdmission& result);

  /// Re-insert persisted events into the bounded event log.
  Status import_events(const std::vector<EventRecord>& events, std::size_t& imported);

  [[nodiscard]] Status inspect(const InspectQuery& query, InspectResult& result) const;
  [[nodiscard]] Status history(const HistoryQuery& query, HistoryResult& result) const;
  [[nodiscard]] Status pressure(const PressureQuery& query, PressureResult& result) const;
  [[nodiscard]] Status explain(const ExplainQuery& query, ExplainResult& result) const;
  [[nodiscard]] Status export_data(const ExportQuery& query, ExportResult& result) const;
  [[nodiscard]] Status events(const EventQuery& query, EventResult& result) const;
  [[nodiscard]] Status contention(const ContentionQuery& query, ContentionResult& result) const;
  [[nodiscard]] Status microburst(const MicroburstQuery& query, MicroburstResult& result) const;
  [[nodiscard]] Status sources(std::vector<SourceRecord>& result) const;
  [[nodiscard]] Status conflicts(std::vector<ConflictRecord>& result) const;

  [[nodiscard]] StoreCounters counters() const;
  [[nodiscard]] SessionInfo session() const;
  [[nodiscard]] std::size_t queue_count() const;
  [[nodiscard]] std::uint64_t history_bytes() const;
  /// Copy of the current metadata table. Returns false in has_metadata when no
  /// revision has been accepted yet.
  [[nodiscard]] Status metadata_snapshot(ClassMetadataTable& result, bool& has_metadata) const;

  /// Bounded scan of the retained history of every queue, used by persistence
  /// and by the exporter. The predicate runs under the shared lock and must not
  /// call back into the store.
  [[nodiscard]] Status for_each_queue(
      const std::function<void(const QueuePath&, const std::vector<HistoryRecord>&)>& visitor,
      std::size_t max_queues) const;

 private:
  struct PeerObservation {
    SourceId source{};
    SourceAuthority authority{SourceAuthority::Unknown};
    std::uint64_t occupancy{0};
    bool has_occupancy{false};
    FieldMask reported{0};
    SampleField occupancy_field{SampleField::Count};
    Nanos received_ns{0};
    ObservationTime observed{};
  };

  struct QueueState {
    QueuePath queue{};
    QueueClassAttributes classes{};
    bool classes_known{false};
    CounterState counters[kSampleFieldCount]{};
    HistoryRing history{};
    ClassificationResult latest{};
    ReceiveTime last_received{};
    ObservationTime last_observed{};
    std::vector<PeerObservation> peers{};
    /// Counter fields this queue has ever reported. A counter that a source
    /// never reports is not counted as a missing delta; one that it reports
    /// sometimes and omits later is.
    FieldMask counters_seen{0};
    /// The exact input that produced the stored classification, retained so
    /// that an explanation can be re-derived under a different policy.
    ClassificationInput last_input{};
    bool class_reference_unsupported{false};
    bool conflict_active{false};
    ReceiveTime conflict_since{};
    std::uint64_t accepted{0};
    std::uint64_t conflicts{0};
  };

  // The exclusive section. All of these run with mutex_ held for writing.
  //
  // The sample is taken by value because the runtime, not the sender, decides
  // when evidence was received: a sample that arrives without a receive time is
  // stamped here with this runtime's clock before anything else looks at it.
  Status ingest_locked(QueueSample sample, AdmissionResult& result);
  QueueState& acquire_queue(const QueuePath& queue, bool& created);
  void record_event(EventKind kind, Severity severity, const QueueSample* sample,
                    const QueuePath& queue, std::string detail);
  void record_event_global(EventKind kind, Severity severity, std::string detail);
  EvidenceAssessment assess(const QueueSample& sample, const SourceRecord& source,
                            bool metadata_derived) const;
  void detect_conflict(QueueState& state, const QueueSample& sample, const SourceRecord& source,
                       AdmissionResult& result);
  void clear_conflict(QueueState& state, AdmissionResult& result);
  std::uint64_t resolve_conflict_tolerance(std::uint64_t value_a, std::uint64_t value_b) const;

  // The shared section.
  void build_inspect_row(const QueueState& state, InspectRow& row) const;
  void build_pressure_row(const QueueState& state, bool include_trace, PressureRow& row) const;
  void build_microburst(const QueueState& state, MicroburstRecord& record) const;
  [[nodiscard]] bool is_contending_state(PressureState state) const;

  PressurePolicy policy_{};
  HistoryLimits history_limits_{};
  QueryLimits query_limits_{};
  std::shared_ptr<Clock> clock_{};

  /// Window specs derived once from the policy. Windows are recomputed at query
  /// time from the bounded history ring, so a queue carries no per-window
  /// allocation and the aggregation cost is bounded by the ring capacity.
  WindowSpec evaluation_spec_{};
  WindowSpec microburst_spec_{};
  WindowSpec contention_spec_{};
  Status config_status_{};

  mutable std::shared_mutex mutex_{};
  std::map<QueuePath, QueueState> queues_{};
  std::map<SourceId, SourceRecord> sources_{};
  ClassMetadataTable metadata_{};
  bool has_metadata_{false};
  bool recovered_mode_{false};
  std::vector<ClassMetadataTable> metadata_history_{};
  std::vector<ConflictRecord> conflicts_{};
  std::vector<EventRecord> event_log_{};
  std::size_t event_head_{0};
  std::size_t event_size_{0};
  IncarnationOrdinal next_incarnation_ordinal_{1};
  std::uint64_t history_bytes_{0};
  StoreCounters counters_{};
  SessionInfo session_{};
};

/// Deterministically generate a runtime identifier from an explicit seed. The
/// identifier is stable for a run and is never random, which keeps exports
/// reproducible.
[[nodiscard]] std::string make_runtime_id(std::uint64_t seed);

}  // namespace qobs
