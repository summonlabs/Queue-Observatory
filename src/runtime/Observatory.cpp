#include "qobs/runtime/Observatory.hpp"

#include <algorithm>
#include <array>

#include "qobs/core/LockAudit.hpp"
#include "qobs/core/Text.hpp"

namespace qobs {

std::string_view to_string(RuntimeState state) noexcept {
  switch (state) {
    case RuntimeState::Created:
      return "created";
    case RuntimeState::Running:
      return "running";
    case RuntimeState::Stopping:
      return "stopping";
    case RuntimeState::Stopped:
      return "stopped";
  }
  return "created";
}

Observatory::Observatory(ObservatoryConfig config, std::shared_ptr<Clock> clock)
    : config_(std::move(config)), clock_(std::move(clock)) {
  store_ = std::make_unique<QueueStore>(config_.policy, config_.history, config_.query, clock_);
  if (config_.persistence_directory.has_value()) {
    persistence_ = std::make_unique<PersistenceStore>(*config_.persistence_directory,
                                                      config_.persistence, clock_);
  }
}

Observatory::~Observatory() {
  // Destruction always stops. A runtime that can outlive its own threads would
  // make shutdown optional, and shutdown is never optional.
  const Status ignored = stop();
  (void)ignored;
}

Result<std::unique_ptr<Observatory>> Observatory::create(ObservatoryConfig config,
                                                         std::shared_ptr<Clock> clock) {
  const std::array<Status, 6> limit_status{validate_limits(config.ingest),
                                           validate_limits(config.history),
                                           validate_limits(config.query),
                                           validate_limits(config.persistence),
                                           validate_limits(config.runtime),
                                           validate_limits(config.transport)};
  for (const Status& status : limit_status) {
    if (!status.ok()) {
      return Result<std::unique_ptr<Observatory>>::failure(status.code(), status.message());
    }
  }
  const Status policy_status = validate_policy(config.policy);
  if (!policy_status.ok()) {
    return Result<std::unique_ptr<Observatory>>::failure(policy_status.code(),
                                                        policy_status.message());
  }
  if (!clock) {
    clock = std::make_shared<SystemClock>();
  }
  auto runtime = std::unique_ptr<Observatory>(new Observatory(std::move(config), std::move(clock)));
  return runtime;
}

bool Observatory::running() const { return state_.load() == RuntimeState::Running; }

Status Observatory::start() {
  std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
  const RuntimeState current = state_.load();
  if (current != RuntimeState::Created && current != RuntimeState::Stopped) {
    return Status::failure(ErrorCode::Busy, "the runtime is already started");
  }
  if (persistence_ != nullptr) {
    QOBS_TRY(persistence_->open());
  }
  stop_source_.reset();
  {
    std::lock_guard<std::mutex> queue_lock(queue_mutex_);
    pending_.clear();
    next_ticket_ = 1;
    next_apply_ticket_ = 1;
    queued_samples_ = 0;
    queued_batches_ = 0;
    in_flight_ = 0;
  }
  state_.store(RuntimeState::Running);

  if (config_.recover_on_start && persistence_ != nullptr) {
    RecoverySummary summary;
    const Status recovery_status = recover(summary);
    if (!recovery_status.ok()) {
      state_.store(RuntimeState::Created);
      return recovery_status;
    }
    recovered_on_start_ = summary.recovered;
    recovery_diagnostic_ = summary.diagnostic;
  }

  const std::size_t threads = config_.runtime.worker_threads;
  workers_.reserve(threads);
  for (std::size_t index = 0; index < threads; ++index) {
    workers_.emplace_back([this] { worker_loop(); });
  }
  return Status::success();
}

Status Observatory::stop() {
  std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
  const RuntimeState current = state_.load();
  if (current == RuntimeState::Stopped || current == RuntimeState::Created) {
    state_.store(RuntimeState::Stopped);
    return Status::success();
  }
  state_.store(RuntimeState::Stopping);
  stop_source_.request_stop();
  {
    std::lock_guard<std::mutex> queue_lock(queue_mutex_);
    queue_cv_.notify_all();
  }
  for (std::thread& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();

  {
    std::lock_guard<std::mutex> queue_lock(queue_mutex_);
    for (auto& entry : pending_) {
      if (!entry.second.applied) {
        ++dropped_documents_;
      }
    }
    pending_.clear();
    in_flight_ = 0;
    queued_samples_ = 0;
    queued_batches_ = 0;
  }
  queue_cv_.notify_all();
  apply_cv_.notify_all();

  if (config_.persist_on_stop && persistence_ != nullptr) {
    PersistenceReport report;
    const Status persist_status = persist_now(report);
    if (!persist_status.ok()) {
      state_.store(RuntimeState::Stopped);
      return persist_status;
    }
  }
  state_.store(RuntimeState::Stopped);
  return Status::success();
}

Status Observatory::drain() {
  std::unique_lock<std::mutex> lock(queue_mutex_);
  apply_cv_.wait(lock, [this] { return pending_.empty() && in_flight_ == 0; });
  return Status::success();
}

Status Observatory::decode_only(PendingDocument& document) {
  return decode_document(document.payload, config_.ingest, document.report.decode);
}

Status Observatory::apply_decoded(PendingDocument& document) {
  if (!document.decode_status.ok()) {
    ++decode_failures_;
    return document.decode_status;
  }
  StopToken token = stop_source_.token();
  for (ClassMetadataTable& table : document.report.decode.metadata) {
    MetadataAdmission admission;
    const Status status = store_->apply_metadata(std::move(table), admission);
    if (!status.ok()) {
      return status;
    }
  }
  const Status status =
      store_->ingest_batch(document.report.decode.samples, token, document.report.admission);
  document.report.accepted = status.ok();
  return status;
}

void Observatory::worker_loop() {
  for (;;) {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    queue_cv_.wait(lock, [this] {
      if (stop_source_.stop_requested()) {
        return true;
      }
      const auto front = pending_.find(next_apply_ticket_);
      if (front != pending_.end() && front->second.decoded) {
        return true;
      }
      for (const auto& entry : pending_) {
        if (!entry.second.decoded && !entry.second.in_flight) {
          return true;
        }
      }
      return false;
    });
    if (stop_source_.stop_requested()) {
      return;
    }

    const auto front = pending_.find(next_apply_ticket_);
    if (front != pending_.end() && front->second.decoded) {
      PendingDocument& document = front->second;
      const Status status = apply_decoded(document);
      (void)status;
      document.applied = true;
      ++applied_documents_;
      pending_.erase(front);
      ++next_apply_ticket_;
      if (queued_batches_ > 0u) {
        --queued_batches_;
      }
      apply_cv_.notify_all();
      queue_cv_.notify_all();
      continue;
    }

    std::uint64_t decode_ticket = 0;
    for (auto& entry : pending_) {
      if (!entry.second.decoded && !entry.second.in_flight) {
        decode_ticket = entry.first;
        break;
      }
    }
    if (decode_ticket == 0) {
      continue;
    }
    auto target = pending_.find(decode_ticket);
    if (target == pending_.end()) {
      continue;
    }
    target->second.in_flight = true;
    ++in_flight_;
    PendingDocument* document = &target->second;
    lock.unlock();
    const Status decode_status = decode_only(*document);
    lock.lock();
    const auto refreshed = pending_.find(decode_ticket);
    if (refreshed != pending_.end()) {
      refreshed->second.decode_status = decode_status;
      refreshed->second.decoded = true;
      refreshed->second.in_flight = false;
      queued_samples_ += refreshed->second.report.decode.samples.size();
    }
    --in_flight_;
    queue_cv_.notify_all();
    apply_cv_.notify_all();
  }
}

Status Observatory::submit_document(std::string payload, std::uint64_t& ticket) {
  ticket = 0;
  if (payload.size() > config_.ingest.max_payload_bytes) {
    return Status::failure(ErrorCode::LimitExceeded,
                           "the document exceeds the configured payload size");
  }
  if (config_.runtime.worker_threads == 0) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (state_.load() != RuntimeState::Running) {
      return Status::failure(ErrorCode::NotRunning,
                             "the runtime must be started before it accepts documents");
    }
    const std::uint64_t assigned = next_ticket_++;
    PendingDocument document;
    document.ticket = assigned;
    document.payload = std::move(payload);
    const Status decode_status = decode_only(document);
    if (decode_status.ok()) {
      const Status apply_status = apply_decoded(document);
      (void)apply_status;
      ++applied_documents_;
    } else {
      ++decode_failures_;
    }
    ticket = assigned;
    return Status::success();
  }
  std::lock_guard<std::mutex> lock(queue_mutex_);
  if (state_.load() != RuntimeState::Running) {
    return Status::failure(ErrorCode::NotRunning,
                           "the runtime must be started before it accepts documents");
  }
  if (pending_.size() >= config_.runtime.max_pending_batches) {
    return Status::failure(ErrorCode::Busy,
                           "the bounded document queue is full; the caller must retry");
  }
  const std::uint64_t assigned = next_ticket_++;
  PendingDocument document;
  document.ticket = assigned;
  document.payload = std::move(payload);
  pending_.emplace(assigned, std::move(document));
  ++queued_batches_;
  queue_cv_.notify_all();
  ticket = assigned;
  return Status::success();
}

Status Observatory::ingest_document(std::string_view payload, IngestReport& report) {
  return qobs::ingest_document(*store_, payload, config_.ingest, report);
}

Status Observatory::ingest_samples(const std::vector<QueueSample>& samples,
                                   BatchAdmission& admission) {
  const StopToken token = stop_source_.token();
  return store_->ingest_batch(samples, token, admission);
}

Status Observatory::apply_metadata(ClassMetadataTable table, MetadataAdmission& admission) {
  return store_->apply_metadata(std::move(table), admission);
}

RuntimeStatus Observatory::status() const {
  RuntimeStatus snapshot;
  snapshot.state = state_.load();
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    snapshot.queued_documents = pending_.size();
    snapshot.queued_samples = queued_samples_;
    snapshot.applied_documents = applied_documents_;
    snapshot.dropped_documents = dropped_documents_;
    snapshot.cancelled_documents = cancelled_documents_;
    snapshot.decode_failures = decode_failures_;
    snapshot.apply_order_violations = apply_order_violations_;
  }
  snapshot.worker_threads = config_.runtime.worker_threads;
  snapshot.runtime_id = store_->session().runtime_id;
  snapshot.started = store_->session().started;
  snapshot.queues = store_->queue_count();
  snapshot.history_bytes = store_->history_bytes();
  snapshot.counters = store_->counters();
  snapshot.recovered_on_start = recovered_on_start_;
  snapshot.recovery_diagnostic = recovery_diagnostic_;
  if (persistence_ != nullptr) {
    snapshot.persistence = persistence_->stats();
  }
  return snapshot;
}

Status Observatory::persist_now(PersistenceReport& report) {
  report = PersistenceReport{};
  if (persistence_ == nullptr) {
    return Status::failure(ErrorCode::NotSupported,
                           "no persistence directory is configured for this runtime");
  }
  DurableSnapshot snapshot;
  QOBS_TRY(build_snapshot(*store_, config_.history.max_samples_per_queue,
                          config_.history.max_events_total, snapshot));
  return persistence_->write_snapshot(snapshot, report);
}

Status Observatory::recover(RecoverySummary& summary) {
  summary = RecoverySummary{};
  if (persistence_ == nullptr) {
    return Status::failure(ErrorCode::NotSupported,
                           "no persistence directory is configured for this runtime");
  }
  summary.attempted = true;
  RecoveryOutcome outcome;
  QOBS_TRY(persistence_->recover(outcome));
  summary.report = outcome.report;
  summary.diagnostic = outcome.diagnostic;
  if (!outcome.recovered) {
    return Status::success();
  }
  return apply_recovered_snapshot(*store_, outcome.snapshot, outcome.report, summary);
}

}  // namespace qobs
