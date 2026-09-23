#include "qobs/model/Event.hpp"

#include "qobs/core/Text.hpp"

namespace qobs {

std::string_view to_string(EventKind kind) noexcept {
  switch (kind) {
    case EventKind::SampleAccepted:
      return "sample_accepted";
    case EventKind::SampleRejected:
      return "sample_rejected";
    case EventKind::SampleFenced:
      return "sample_fenced";
    case EventKind::DuplicateReplay:
      return "duplicate_replay";
    case EventKind::ConflictDetected:
      return "conflict_detected";
    case EventKind::ConflictCleared:
      return "conflict_cleared";
    case EventKind::CounterWrap:
      return "counter_wrap";
    case EventKind::CounterReset:
      return "counter_reset";
    case EventKind::CounterAmbiguous:
      return "counter_ambiguous";
    case EventKind::CounterDiscontinuous:
      return "counter_discontinuous";
    case EventKind::SourceRegistered:
      return "source_registered";
    case EventKind::SourceIncarnationChanged:
      return "source_incarnation_changed";
    case EventKind::SourceAuthorityChanged:
      return "source_authority_changed";
    case EventKind::SourceSequenceRegression:
      return "source_sequence_regression";
    case EventKind::MetadataUpdated:
      return "metadata_updated";
    case EventKind::MetadataRevisionFenced:
      return "metadata_revision_fenced";
    case EventKind::StateTransition:
      return "state_transition";
    case EventKind::MicroburstDetected:
      return "microburst_detected";
    case EventKind::MicroburstInsufficientCoverage:
      return "microburst_insufficient_coverage";
    case EventKind::ContentionDetected:
      return "contention_detected";
    case EventKind::PersistenceWritten:
      return "persistence_written";
    case EventKind::PersistenceRecovered:
      return "persistence_recovered";
    case EventKind::PersistenceFailed:
      return "persistence_failed";
    case EventKind::HistoryEvicted:
      return "history_evicted";
    case EventKind::QueueLimitReached:
      return "queue_limit_reached";
    case EventKind::RuntimeStarted:
      return "runtime_started";
    case EventKind::RuntimeStopped:
      return "runtime_stopped";
  }
  return "unmapped";
}

std::string_view to_string(Severity value) noexcept {
  switch (value) {
    case Severity::Debug:
      return "debug";
    case Severity::Info:
      return "info";
    case Severity::Notice:
      return "notice";
    case Severity::Warning:
      return "warning";
    case Severity::Error:
      return "error";
  }
  return "info";
}

std::string render_event(const EventRecord& event) {
  std::string out;
  out.reserve(160u + event.detail.size());
  out.append(format_wall_utc(event.received.wall));
  out.push_back(' ');
  out.append(to_string(event.severity));
  out.push_back(' ');
  out.append(to_string(event.kind));
  if (event.has_queue()) {
    out.append(" queue=");
    out.append(event.queue.to_string());
  }
  if (event.source.valid()) {
    out.append(" source=");
    out.append(text::sanitize_for_display(event.source.view(), 64));
  }
  if (event.incarnation.valid()) {
    out.append(" incarnation=");
    out.append(text::sanitize_for_display(event.incarnation.view(), 64));
  }
  if (event.sequence.valid()) {
    out.append(" sequence=");
    out.append(event.sequence.to_string());
  }
  if (event.revision.valid()) {
    out.append(" revision=");
    out.append(event.revision.to_string());
  }
  if (!event.detail.empty()) {
    out.append(" detail=");
    out.append(text::sanitize_for_display(event.detail, 512));
  }
  return out;
}

}  // namespace qobs
