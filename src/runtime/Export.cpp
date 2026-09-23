#include "qobs/runtime/Report.hpp"

#include <sstream>

#include "qobs/core/Text.hpp"

namespace qobs {
namespace {

std::string state_column(PressureState state) { return std::string(to_string(state)); }

void append_row(std::string& out, const std::vector<std::string>& cells,
                const std::vector<std::size_t>& widths) {
  for (std::size_t index = 0; index < cells.size(); ++index) {
    if (index != 0) {
      out.push_back(' ');
    }
    out.append(text::pad_right(cells[index], widths[index]));
  }
  out.push_back('\n');
}

}  // namespace

std::string render_inspect_table(const InspectResult& result, bool header) {
  const std::vector<std::size_t> widths{28u, 8u, 12u, 12u, 10u, 8u, 8u, 8u, 8u, 8u};
  std::string out;
  if (header) {
    append_row(out, {"QUEUE", "STATE", "FRESHNESS", "QUALITY", "RECORDS", "WRAPS", "RESETS",
                     "AMBIG", "DISCONT", "ACCEPTED"},
               widths);
  }
  for (const InspectRow& row : result.rows) {
    append_row(out,
               {text::sanitize_for_display(row.queue.to_string(), 28u), state_column(row.state),
                std::string(to_string(row.assessment.freshness)),
                std::string(to_string(row.assessment.quality)),
                std::to_string(row.history_records), std::to_string(row.counter_wraps),
                std::to_string(row.counter_resets), std::to_string(row.counter_ambiguous),
                std::to_string(row.counter_discontinuous), std::to_string(row.accepted)},
               widths);
  }
  out.append("matched=");
  out.append(std::to_string(result.total_matched));
  out.append(" returned=");
  out.append(std::to_string(result.rows.size()));
  out.append(" truncated=");
  out.append(result.truncated ? "true" : "false");
  out.push_back('\n');
  return out;
}

std::string render_pressure_table(const PressureResult& result, bool header) {
  const std::vector<std::size_t> widths{28u, 12u, 22u, 14u, 14u, 16u};
  std::string out;
  if (header) {
    append_row(out, {"QUEUE", "STATE", "BASIS", "VALUE", "LIMIT", "DECIDING_RULE"}, widths);
  }
  for (const PressureRow& row : result.rows) {
    append_row(out,
               {text::sanitize_for_display(row.queue.to_string(), 28u), state_column(row.state),
                text::sanitize_for_display(row.basis.label, 22u),
                row.basis.value.has_value() ? std::to_string(*row.basis.value)
                                            : std::string("not_reported"),
                row.basis.limit.has_value() ? std::to_string(*row.basis.limit)
                                            : std::string("not_reported"),
                std::string(rule_name(row.deciding_rule))},
               widths);
  }
  out.append("matched=");
  out.append(std::to_string(result.total_matched));
  out.append(" returned=");
  out.append(std::to_string(result.rows.size()));
  out.append(" truncated=");
  out.append(result.truncated ? "true" : "false");
  out.push_back('\n');
  return out;
}

std::string render_history_report(const HistoryResult& result) {
  std::string out;
  out.append("queue=");
  out.append(result.queue.to_string());
  out.append(" found=");
  out.append(result.found ? "true" : "false");
  out.push_back('\n');
  if (!result.found) {
    return out;
  }
  const WindowAggregate& aggregate = result.aggregate;
  out.append("window=");
  out.append(aggregate.spec.name);
  out.append(" buckets=");
  out.append(std::to_string(aggregate.bucket_count));
  out.append(" covered=");
  out.append(std::to_string(aggregate.covered_buckets));
  out.append(" coverage_sufficient=");
  out.append(aggregate.coverage_sufficient ? "true" : "false");
  out.push_back('\n');
  out.append("samples=");
  out.append(std::to_string(aggregate.sample_count));
  out.append(" occupancy_max=");
  out.append(aggregate.occupancy_max.has_value() ? std::to_string(*aggregate.occupancy_max)
                                                 : std::string("not_reported"));
  out.append(" occupancy_min=");
  out.append(aggregate.occupancy_min.has_value() ? std::to_string(*aggregate.occupancy_min)
                                                 : std::string("not_reported"));
  out.append(" occupancy_last=");
  out.append(aggregate.occupancy_last.has_value() ? std::to_string(*aggregate.occupancy_last)
                                                  : std::string("not_reported"));
  out.append(" excursion=");
  out.append(std::to_string(aggregate.occupancy_excursion));
  out.push_back('\n');
  out.append("drops=");
  out.append(std::to_string(aggregate.drops));
  out.append(" marks=");
  out.append(std::to_string(aggregate.marks));
  out.append(" pause_frames=");
  out.append(std::to_string(aggregate.pause_frames));
  out.append(" pause_nanos=");
  out.append(std::to_string(aggregate.pause_nanos));
  out.append(" unknown_delta_samples=");
  out.append(std::to_string(aggregate.unknown_delta_samples));
  out.push_back('\n');
  out.append("assessment=");
  out.append(describe(aggregate.assessment));
  out.push_back('\n');
  out.append("records_retained=");
  out.append(std::to_string(result.records_retained));
  out.append(" records_evicted=");
  out.append(std::to_string(result.records_evicted));
  out.append(" records_returned=");
  out.append(std::to_string(result.records.size()));
  out.append(" truncated=");
  out.append(result.truncated ? "true" : "false");
  out.push_back('\n');
  for (const HistoryRecord& record : result.records) {
    out.append("  ");
    out.append(format_wall_utc(record.received.wall));
    out.append(" state=");
    out.append(to_string(record.state));
    out.append(" freshness=");
    out.append(to_string(record.freshness));
    out.append(" quality=");
    out.append(to_string(record.quality));
    out.append(" source=");
    out.append(text::sanitize_for_display(record.source.view(), 32u));
    out.append(" sequence=");
    out.append(record.sequence.to_string());
    if (const auto cells = record.value(SampleField::OccupancyCells); cells.has_value()) {
      out.append(" occupancy_cells=");
      out.append(std::to_string(*cells));
    }
    if (const auto drops = record.delta(SampleField::DropPackets); drops.known) {
      out.append(" drop_delta=");
      out.append(std::to_string(drops.delta));
    }
    out.push_back('\n');
  }
  return out;
}

std::string render_microburst_report(const MicroburstResult& result) {
  std::string out;
  out.append("queues_examined=");
  out.append(std::to_string(result.queues_examined));
  out.append(" queues_insufficient_coverage=");
  out.append(std::to_string(result.queues_insufficient));
  out.append(" records=");
  out.append(std::to_string(result.records.size()));
  out.append(" truncated=");
  out.append(result.truncated ? "true" : "false");
  out.push_back('\n');
  for (const MicroburstRecord& record : result.records) {
    out.append(text::sanitize_for_display(record.queue.to_string(), 28u));
    out.append(" detected=");
    out.append(record.detected ? "true" : "false");
    out.append(" excursion=");
    out.append(std::to_string(record.excursion));
    out.append(" required=");
    out.append(std::to_string(record.required_excursion));
    out.append(" peak=");
    out.append(std::to_string(record.peak));
    out.append(" trough=");
    out.append(std::to_string(record.trough));
    out.append(" covered_buckets=");
    out.append(std::to_string(record.covered_buckets));
    out.append("/");
    out.append(std::to_string(record.required_buckets));
    out.append(" coverage_sufficient=");
    out.append(record.coverage_sufficient ? "true" : "false");
    out.append(" sub_sample_reconstructed=false");
    out.push_back('\n');
  }
  return out;
}

std::string render_contention_report(const ContentionResult& result) {
  std::string out;
  out.append("ports_examined=");
  out.append(std::to_string(result.ports_examined));
  out.append(" pairs_examined=");
  out.append(std::to_string(result.pairs_examined));
  out.append(" pairs_insufficient_coverage=");
  out.append(std::to_string(result.pairs_insufficient_coverage));
  out.append(" records=");
  out.append(std::to_string(result.records.size()));
  out.append(" truncated=");
  out.append(result.truncated ? "true" : "false");
  out.push_back('\n');
  for (const ContentionRecord& record : result.records) {
    out.append(record.port.to_string());
    out.append(" class=");
    out.append(text::sanitize_for_display(record.scheduling_class.view(), 16u));
    out.append(" a=");
    out.append(record.queue_a.to_string());
    out.append(" b=");
    out.append(record.queue_b.to_string());
    out.append(" ratio=");
    out.append(std::to_string(record.ratio_numerator));
    out.append("/");
    out.append(std::to_string(record.ratio_denominator));
    out.append(" simultaneous=");
    out.append(std::to_string(record.simultaneous_buckets));
    out.append(" covered=");
    out.append(std::to_string(record.covered_buckets));
    out.push_back('\n');
  }
  return out;
}

std::string render_events_report(const EventResult& result) {
  std::string out;
  for (const EventRecord& event : result.events) {
    out.append(render_event(event));
    out.push_back('\n');
  }
  out.append("matched=");
  out.append(std::to_string(result.total_matched));
  out.append(" returned=");
  out.append(std::to_string(result.events.size()));
  out.append(" truncated=");
  out.append(result.truncated ? "true" : "false");
  out.push_back('\n');
  return out;
}

std::string render_sources_report(const std::vector<SourceRecord>& sources) {
  std::string out;
  for (const SourceRecord& source : sources) {
    out.append(text::sanitize_for_display(source.id.view(), 32u));
    out.append(" authority=");
    out.append(to_string(source.authority));
    out.append(" incarnation=");
    out.append(text::sanitize_for_display(source.incarnation.view(), 32u));
    out.append(" ordinal=");
    out.append(std::to_string(source.incarnation_ordinal));
    out.append(" generation=");
    out.append(source.generation.to_string());
    out.append(" last_sequence=");
    out.append(source.last_sequence.to_string());
    out.append(" accepted=");
    out.append(std::to_string(source.accepted));
    out.append(" fenced=");
    out.append(std::to_string(source.fenced));
    out.append(" duplicates=");
    out.append(std::to_string(source.duplicates));
    out.push_back('\n');
  }
  out.append("sources=");
  out.append(std::to_string(sources.size()));
  out.push_back('\n');
  return out;
}

std::string render_conflicts_report(const std::vector<ConflictRecord>& conflicts) {
  std::string out;
  for (const ConflictRecord& conflict : conflicts) {
    out.append(format_wall_utc(conflict.detected.wall));
    out.append(" queue=");
    out.append(conflict.queue.to_string());
    out.append(" a=");
    out.append(text::sanitize_for_display(conflict.source_a.view(), 32u));
    out.append(" b=");
    out.append(text::sanitize_for_display(conflict.source_b.view(), 32u));
    out.append(" value_a=");
    out.append(std::to_string(conflict.value_a));
    out.append(" value_b=");
    out.append(std::to_string(conflict.value_b));
    out.append(" difference=");
    out.append(std::to_string(conflict.difference));
    out.append(" tolerance=");
    out.append(std::to_string(conflict.tolerance));
    out.append(" active=");
    out.append(conflict.active ? "true" : "false");
    out.push_back('\n');
  }
  out.append("conflicts=");
  out.append(std::to_string(conflicts.size()));
  out.push_back('\n');
  return out;
}

std::string render_ingest_report(const IngestReport& report) {
  std::string out;
  out.append("lines=");
  out.append(std::to_string(report.decode.lines));
  out.append(" records=");
  out.append(std::to_string(report.decode.records));
  out.append(" samples=");
  out.append(std::to_string(report.decode.sample_count));
  out.append(" metadata_records=");
  out.append(std::to_string(report.decode.metadata_records));
  out.append(" skipped=");
  out.append(std::to_string(report.decode.skipped));
  out.append(" malformed=");
  out.append(std::to_string(report.decode.malformed));
  out.append(" unsupported=");
  out.append(std::to_string(report.decode.unsupported));
  out.append(" version_mismatches=");
  out.append(std::to_string(report.decode.version_mismatches));
  out.append(" unknown_keys=");
  out.append(std::to_string(report.decode.unknown_keys));
  out.push_back('\n');
  out.append("presented=");
  out.append(std::to_string(report.admission.presented));
  out.append(" accepted=");
  out.append(std::to_string(report.admission.accepted));
  out.append(" rejected=");
  out.append(std::to_string(report.admission.rejected));
  out.append(" fenced=");
  out.append(std::to_string(report.admission.fenced));
  out.append(" duplicates=");
  out.append(std::to_string(report.admission.duplicates));
  out.append(" conflicts=");
  out.append(std::to_string(report.admission.conflicts));
  out.append(" state_changes=");
  out.append(std::to_string(report.admission.state_changes));
  out.append(" queues_created=");
  out.append(std::to_string(report.admission.queues_created));
  out.push_back('\n');
  for (const DecodeDiagnostic& diagnostic : report.decode.diagnostics) {
    out.append("  line=");
    out.append(std::to_string(diagnostic.line));
    out.append(" status=");
    out.append(to_string(diagnostic.status));
    out.append(" reason=");
    out.append(text::sanitize_for_display(diagnostic.reason, 256u));
    out.push_back('\n');
  }
  if (report.decode.diagnostics_truncated) {
    out.append("  diagnostics_truncated=true\n");
  }
  for (const AdmissionFailure& failure : report.admission.failures) {
    out.append("  index=");
    out.append(std::to_string(failure.index));
    out.append(" outcome=");
    out.append(to_string(failure.outcome));
    out.append(" queue=");
    out.append(failure.queue.to_string());
    out.append(" reason=");
    out.append(text::sanitize_for_display(failure.reason, 256u));
    out.push_back('\n');
  }
  if (report.admission.failures_truncated) {
    out.append("  admission_failures_truncated=true\n");
  }
  return out;
}

std::string render_runtime_status(const RuntimeStatus& status) {
  std::string out;
  out.append("state=");
  out.append(to_string(status.state));
  out.append(" runtime_id=");
  out.append(text::sanitize_for_display(status.runtime_id, 32u));
  out.append(" workers=");
  out.append(std::to_string(status.worker_threads));
  out.append(" queued_documents=");
  out.append(std::to_string(status.queued_documents));
  out.append(" queued_samples=");
  out.append(std::to_string(status.queued_samples));
  out.append(" applied_documents=");
  out.append(std::to_string(status.applied_documents));
  out.append(" dropped_documents=");
  out.append(std::to_string(status.dropped_documents));
  out.append(" decode_failures=");
  out.append(std::to_string(status.decode_failures));
  out.append(" apply_order_violations=");
  out.append(std::to_string(status.apply_order_violations));
  out.push_back('\n');
  out.append("queues=");
  out.append(std::to_string(status.queues));
  out.append(" history_bytes=");
  out.append(std::to_string(status.history_bytes));
  out.append(" samples_presented=");
  out.append(std::to_string(status.counters.samples_presented));
  out.append(" samples_accepted=");
  out.append(std::to_string(status.counters.samples_accepted));
  out.append(" samples_rejected=");
  out.append(std::to_string(status.counters.samples_rejected));
  out.append(" samples_fenced=");
  out.append(std::to_string(status.counters.samples_fenced));
  out.append(" duplicates_suppressed=");
  out.append(std::to_string(status.counters.duplicates_suppressed));
  out.append(" counter_wraps=");
  out.append(std::to_string(status.counters.counter_wraps));
  out.append(" counter_resets=");
  out.append(std::to_string(status.counters.counter_resets));
  out.append(" counter_ambiguous=");
  out.append(std::to_string(status.counters.counter_ambiguous));
  out.append(" counter_discontinuous=");
  out.append(std::to_string(status.counters.counter_discontinuous));
  out.push_back('\n');
  out.append("recovered_on_start=");
  out.append(status.recovered_on_start ? "true" : "false");
  if (!status.recovery_diagnostic.empty()) {
    out.append(" recovery_diagnostic=");
    out.append(text::sanitize_for_display(status.recovery_diagnostic, 256u));
  }
  out.push_back('\n');
  return out;
}

std::string render_recovery_summary(const RecoverySummary& summary) {
  std::string out;
  out.append("attempted=");
  out.append(summary.attempted ? "true" : "false");
  out.append(" recovered=");
  out.append(summary.recovered ? "true" : "false");
  out.append(" queues=");
  out.append(std::to_string(summary.queues));
  out.append(" samples=");
  out.append(std::to_string(summary.samples));
  out.append(" presented=");
  out.append(std::to_string(summary.samples_presented));
  out.append(" rejected=");
  out.append(std::to_string(summary.samples_rejected));
  out.append(" fenced=");
  out.append(std::to_string(summary.samples_fenced));
  out.append(" events=");
  out.append(std::to_string(summary.events));
  out.append(" metadata_present=");
  out.append(summary.metadata_present ? "true" : "false");
  out.append(" evidence_marked_stale=");
  out.append(summary.evidence_marked_stale ? "true" : "false");
  out.push_back('\n');
  out.append("format_version=");
  out.append(std::to_string(summary.report.format_version));
  out.append(" header_crc_valid=");
  out.append(summary.report.header_crc_valid ? "true" : "false");
  out.append(" payload_crc_valid=");
  out.append(summary.report.payload_crc_valid ? "true" : "false");
  out.append(" truncated=");
  out.append(summary.report.truncated ? "true" : "false");
  out.append(" integrity_failed=");
  out.append(summary.report.integrity_failed ? "true" : "false");
  out.append(" records_parsed=");
  out.append(std::to_string(summary.report.records_parsed));
  out.append(" records_declared=");
  out.append(std::to_string(summary.report.records_declared));
  out.push_back('\n');
  if (!summary.diagnostic.empty()) {
    out.append("diagnostic=");
    out.append(text::sanitize_for_display(summary.diagnostic, 256u));
    out.push_back('\n');
  }
  return out;
}

}  // namespace qobs
