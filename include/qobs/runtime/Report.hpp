#pragma once

#include <string>
#include <vector>

#include "qobs/runtime/Observatory.hpp"

namespace qobs {

/// Deterministic text renderers used by the CLI and by tests.
///
/// Every renderer produces a stable, line-oriented form with a column header on
/// request, so that output can be compared byte for byte between runs.
[[nodiscard]] std::string render_inspect_table(const InspectResult& result, bool header);
[[nodiscard]] std::string render_pressure_table(const PressureResult& result, bool header);
[[nodiscard]] std::string render_history_report(const HistoryResult& result);
[[nodiscard]] std::string render_microburst_report(const MicroburstResult& result);
[[nodiscard]] std::string render_contention_report(const ContentionResult& result);
[[nodiscard]] std::string render_events_report(const EventResult& result);
[[nodiscard]] std::string render_sources_report(const std::vector<SourceRecord>& sources);
[[nodiscard]] std::string render_conflicts_report(const std::vector<ConflictRecord>& conflicts);
[[nodiscard]] std::string render_ingest_report(const IngestReport& report);
[[nodiscard]] std::string render_runtime_status(const RuntimeStatus& status);
[[nodiscard]] std::string render_recovery_summary(const RecoverySummary& summary);

}  // namespace qobs
