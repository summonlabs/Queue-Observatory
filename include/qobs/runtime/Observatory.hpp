#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "qobs/core/Cancellation.hpp"
#include "qobs/core/Limits.hpp"
#include "qobs/core/Result.hpp"
#include "qobs/core/Time.hpp"
#include "qobs/ingest/Ingest.hpp"
#include "qobs/persist/Persistence.hpp"
#include "qobs/store/Store.hpp"

namespace qobs {

struct ObservatoryConfig {
  PressurePolicy policy{};
  IngestLimits ingest{};
  HistoryLimits history{};
  QueryLimits query{};
  PersistenceLimits persistence{};
  RuntimeLimits runtime{};
  TransportLimits transport{};
  /// When set, snapshots can be written to and recovered from this directory.
  std::optional<std::filesystem::path> persistence_directory{};
  /// Recover persisted evidence during start().
  bool recover_on_start{true};
  /// Write a snapshot whenever stop() is called with a drained queue.
  bool persist_on_stop{false};
  /// Explicit seed for the runtime identifier. Zero derives it from the wall
  /// clock at start.
  std::uint64_t runtime_seed{0};

  friend bool operator==(const ObservatoryConfig&, const ObservatoryConfig&) = default;
};

enum class RuntimeState : std::uint8_t { Created = 0, Running, Stopping, Stopped };

[[nodiscard]] std::string_view to_string(RuntimeState state) noexcept;

struct RuntimeStatus {
  RuntimeState state{RuntimeState::Created};
  std::string runtime_id{};
  ReceiveTime started{};
  std::size_t worker_threads{0};
  std::size_t queued_documents{0};
  std::size_t queued_samples{0};
  std::size_t applied_documents{0};
  std::size_t dropped_documents{0};
  std::size_t cancelled_documents{0};
  std::size_t decode_failures{0};
  std::size_t queues{0};
  std::uint64_t history_bytes{0};
  StoreCounters counters{};
  PersistenceReport persistence{};
  bool recovered_on_start{false};
  std::string recovery_diagnostic{};
  std::uint64_t apply_order_violations{0};
};

/// What recovery did, expressed in the vocabulary of the observation model.
struct RecoverySummary {
  bool attempted{false};
  bool recovered{false};
  std::size_t queues{0};
  std::size_t samples_presented{0};
  std::size_t samples{0};
  std::size_t samples_rejected{0};
  std::size_t samples_fenced{0};
  std::size_t events{0};
  std::size_t sources{0};
  bool metadata_present{false};
  DecodeReport report{};
  std::string diagnostic{};
  /// Persisted dynamic evidence is re-admitted as history with the recovered
  /// provenance. It can never be reported as fresh and therefore never defines
  /// the current state of a queue.
  bool evidence_marked_stale{true};
};

/// The top-level runtime.
///
/// Threading contract: a bounded pool of workers decodes submitted documents in
/// parallel and applies them to the store in submission order. The order of
/// application therefore does not depend on the number of workers, which is
/// what makes the runtime's output reproducible.
///
/// Locking contract: the store's shared mutex is the only lock held while
/// applying evidence, it is always acquired at rank kRankStore, and no callback
/// is invoked while it is held.
class Observatory {
 public:
  [[nodiscard]] static Result<std::unique_ptr<Observatory>> create(ObservatoryConfig config,
                                                                   std::shared_ptr<Clock> clock);

  ~Observatory();
  Observatory(const Observatory&) = delete;
  Observatory& operator=(const Observatory&) = delete;
  Observatory(Observatory&&) = delete;
  Observatory& operator=(Observatory&&) = delete;

  [[nodiscard]] const ObservatoryConfig& config() const noexcept { return config_; }
  [[nodiscard]] QueueStore& store() noexcept { return *store_; }
  [[nodiscard]] const QueueStore& store() const noexcept { return *store_; }
  [[nodiscard]] StopToken stop_token() const noexcept { return stop_source_.token(); }

  [[nodiscard]] Status start();
  /// Request cancellation, wake every worker, discard queued documents and join
  /// every thread. Returns once no worker is running.
  [[nodiscard]] Status stop();
  /// Wait until every submitted document has been applied or discarded.
  [[nodiscard]] Status drain();
  [[nodiscard]] bool running() const;

  /// Submit a document for asynchronous handling. Returns Busy when the bounded
  /// queue is full and a ticket that drain() can wait on.
  [[nodiscard]] Status submit_document(std::string payload, std::uint64_t& ticket);

  /// Decode and apply a document synchronously. Deterministic and independent
  /// of the worker pool.
  [[nodiscard]] Status ingest_document(std::string_view payload, IngestReport& report);

  [[nodiscard]] Status ingest_samples(const std::vector<QueueSample>& samples,
                                      BatchAdmission& admission);
  [[nodiscard]] Status apply_metadata(ClassMetadataTable table, MetadataAdmission& admission);

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
  [[nodiscard]] RuntimeStatus status() const;

  [[nodiscard]] Status persist_now(PersistenceReport& report);
  [[nodiscard]] Status recover(RecoverySummary& summary);

 private:
  Observatory(ObservatoryConfig config, std::shared_ptr<Clock> clock);

  struct PendingDocument {
    std::uint64_t ticket{0};
    std::string payload{};
    bool in_flight{false};
    bool decoded{false};
    bool applied{false};
    Status decode_status{};
    IngestReport report{};
  };

  void worker_loop();
  [[nodiscard]] Status decode_only(PendingDocument& document);
  [[nodiscard]] Status apply_decoded(PendingDocument& document);

  ObservatoryConfig config_{};
  std::shared_ptr<Clock> clock_{};
  std::unique_ptr<QueueStore> store_{};
  std::unique_ptr<PersistenceStore> persistence_{};

  mutable std::mutex queue_mutex_{};
  std::condition_variable queue_cv_{};
  std::condition_variable apply_cv_{};
  std::map<std::uint64_t, PendingDocument> pending_{};
  std::uint64_t next_ticket_{1};
  std::uint64_t next_apply_ticket_{1};
  std::size_t queued_samples_{0};
  std::size_t queued_batches_{0};
  std::size_t applied_documents_{0};
  std::size_t dropped_documents_{0};
  std::size_t cancelled_documents_{0};
  std::size_t decode_failures_{0};
  std::size_t in_flight_{0};
  std::uint64_t apply_order_violations_{0};

  StopSource stop_source_{};
  mutable std::mutex lifecycle_mutex_{};
  std::atomic<RuntimeState> state_{RuntimeState::Created};
  std::vector<std::thread> workers_{};
  bool recovered_on_start_{false};
  std::string recovery_diagnostic_{};
};

/// Re-admit a decoded snapshot into a store. Samples that were persisted while
/// a queue was healthy are re-admitted as stale evidence: their receive times
/// belong to a previous process and are not comparable with the new steady
/// clock, so they can never define the current state.
[[nodiscard]] Status apply_recovered_snapshot(QueueStore& store, const DurableSnapshot& snapshot,
                                              const DecodeReport& report,
                                              RecoverySummary& summary);

}  // namespace qobs
