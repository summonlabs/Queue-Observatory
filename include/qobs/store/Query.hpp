#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "qobs/core/Result.hpp"
#include "qobs/model/Event.hpp"
#include "qobs/model/Identity.hpp"
#include "qobs/policy/Classification.hpp"
#include "qobs/policy/Explanation.hpp"
#include "qobs/store/History.hpp"
#include "qobs/store/Window.hpp"

namespace qobs {

/// Every query result set is paged. A query that would exceed the page size
/// reports the true match count and sets truncated, so a caller can never
/// mistake a bounded answer for a complete one.
struct QueryPagination {
  std::size_t offset{0};
  /// Zero selects the configured default page size.
  std::size_t limit{0};

  friend bool operator==(const QueryPagination&, const QueryPagination&) = default;
};

/// Selection criteria shared by inspect, pressure, microburst and export.
struct QueueFilter {
  std::optional<DeviceId> device{};
  std::optional<PortId> port{};
  std::optional<QueueId> queue{};
  std::optional<TrafficClassId> traffic_class{};
  std::optional<SchedulingClassId> scheduling_class{};
  std::optional<PressureState> state{};

  friend bool operator==(const QueueFilter&, const QueueFilter&) = default;
};

struct InspectQuery {
  QueueFilter filter{};
  QueryPagination page{};
};

struct InspectRow {
  QueuePath queue{};
  QueueClassAttributes classes{};
  PressureState state{PressureState::Unknown};
  EvidenceAssessment assessment{};
  /// Canonical single-line rendering of the decision.
  std::string explanation_summary{};
  ReceiveTime last_received{};
  ObservationTime last_observed{};
  std::size_t history_records{0};
  std::size_t history_capacity{0};
  std::uint64_t history_evicted{0};
  std::uint64_t accepted{0};
  std::uint64_t conflicts{0};
  std::uint64_t counter_wraps{0};
  std::uint64_t counter_resets{0};
  std::uint64_t counter_ambiguous{0};
  std::uint64_t counter_discontinuous{0};
  bool conflict_active{false};
  /// True when the sample named a class that the current metadata table does
  /// not describe. The record is still reported; class-level correlation is
  /// simply not claimed for it.
  bool class_reference_unsupported{false};
};

struct InspectResult {
  std::vector<InspectRow> rows{};
  std::size_t total_matched{0};
  bool truncated{false};
};

struct HistoryQuery {
  QueuePath queue{};
  WindowSpec window{};
  QueryPagination page{};
  /// When false only the aggregate is returned, which keeps the response small.
  bool include_records{true};
};

struct HistoryResult {
  QueuePath queue{};
  bool found{false};
  WindowAggregate aggregate{};
  std::vector<HistoryRecord> records{};
  std::size_t records_retained{0};
  std::size_t records_total{0};
  std::uint64_t records_evicted{0};
  bool truncated{false};
};

struct PressureQuery {
  QueueFilter filter{};
  QueryPagination page{};
  /// When true each row carries the full decision trace.
  bool include_trace{false};
};

struct PressureRow {
  QueuePath queue{};
  QueueClassAttributes classes{};
  PressureState state{PressureState::Unknown};
  EvidenceAssessment assessment{};
  PressureBasis basis{};
  RuleId deciding_rule{RuleId::EvidencePresence};
  std::uint64_t explanation_digest{0};
  std::vector<TraceEntry> trace{};
};

struct PressureResult {
  std::vector<PressureRow> rows{};
  std::size_t total_matched{0};
  bool truncated{false};
};

struct ExplainQuery {
  QueuePath queue{};
  /// Optional policy override used to re-derive the decision. When empty the
  /// policy that produced the stored state is used.
  std::optional<PressurePolicy> policy{};
};

struct ExplainResult {
  QueuePath queue{};
  bool found{false};
  ClassificationResult classification{};
  std::string explanation{};
  std::vector<ExplanationLine> lines{};
  /// True when a caller-supplied policy was applied instead of the stored one.
  bool policy_overridden{false};
};

enum class ExportFormat : std::uint8_t { Ndjson = 0, Text };

[[nodiscard]] std::string_view to_string(ExportFormat format) noexcept;
[[nodiscard]] std::optional<ExportFormat> parse_export_format(std::string_view text) noexcept;

struct ExportQuery {
  QueueFilter filter{};
  QueryPagination page{};
  ExportFormat format{ExportFormat::Ndjson};
  /// Include the retained history records of each matched queue.
  bool include_history{false};
  WindowSpec window{};
};

struct ExportResult {
  std::string document{};
  std::size_t rows{0};
  std::size_t history_records{0};
  bool truncated{false};
};

struct EventQuery {
  std::optional<EventKind> kind{};
  std::optional<QueuePath> queue{};
  std::optional<Severity> min_severity{};
  QueryPagination page{};
};

struct EventResult {
  std::vector<EventRecord> events{};
  std::size_t total_matched{0};
  bool truncated{false};
};

/// Filter predicate shared by the store and the exporter.
[[nodiscard]] bool matches_filter(const QueueFilter& filter, const QueuePath& queue,
                                  const QueueClassAttributes& classes, PressureState state);

}  // namespace qobs
