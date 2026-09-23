#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "qobs/core/StrongId.hpp"
#include "qobs/core/Time.hpp"
#include "qobs/model/Evidence.hpp"
#include "qobs/model/Identity.hpp"

namespace qobs {

/// Everything the runtime decided about an observation is an event. Events are
/// the audit trail that makes a past conclusion explainable.
enum class EventKind : std::uint8_t {
  SampleAccepted = 0,
  SampleRejected,
  SampleFenced,
  DuplicateReplay,
  ConflictDetected,
  ConflictCleared,
  CounterWrap,
  CounterReset,
  CounterAmbiguous,
  CounterDiscontinuous,
  SourceRegistered,
  SourceIncarnationChanged,
  SourceAuthorityChanged,
  SourceSequenceRegression,
  MetadataUpdated,
  MetadataRevisionFenced,
  StateTransition,
  MicroburstDetected,
  MicroburstInsufficientCoverage,
  ContentionDetected,
  PersistenceWritten,
  PersistenceRecovered,
  PersistenceFailed,
  HistoryEvicted,
  QueueLimitReached,
  RuntimeStarted,
  RuntimeStopped,
};

inline constexpr std::size_t kEventKindCount = static_cast<std::size_t>(EventKind::RuntimeStopped) + 1u;

[[nodiscard]] std::string_view to_string(EventKind kind) noexcept;

enum class Severity : std::uint8_t { Debug = 0, Info, Notice, Warning, Error };

[[nodiscard]] std::string_view to_string(Severity value) noexcept;

/// One entry in the bounded event log.
struct EventRecord {
  EventKind kind{EventKind::SampleAccepted};
  Severity severity{Severity::Info};
  ReceiveTime received{};
  ObservationTime observed{};
  /// Empty for runtime-wide events such as start and stop.
  QueuePath queue{};
  SourceId source{};
  IncarnationId incarnation{};
  SourceSequence sequence{};
  Revision revision{};
  /// Short, deterministic, machine-stable detail string. Never contains a
  /// pointer value, an address, or a localized message.
  std::string detail{};

  [[nodiscard]] bool has_queue() const noexcept { return queue.valid(); }
};

/// Deterministic single-line rendering used by the CLI and by export.
[[nodiscard]] std::string render_event(const EventRecord& event);

}  // namespace qobs
