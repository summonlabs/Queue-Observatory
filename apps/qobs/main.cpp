// Queue Observatory command line interface.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0
//
// The command line exposes observation and interpretation only. There is no
// subcommand that schedules, shapes, marks, pauses, or otherwise changes any
// device, queue or buffer.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "qobs/core/Checked.hpp"
#include "qobs/core/Json.hpp"
#include "qobs/core/Text.hpp"
#include "qobs/ingest/Ingest.hpp"
#include "qobs/runtime/Observatory.hpp"
#include "qobs/runtime/Report.hpp"

namespace {

struct Options {
  std::string command{};
  std::vector<std::pair<std::string, std::string>> values{};
  std::vector<std::string> positional{};

  [[nodiscard]] std::optional<std::string> get(const std::string& name) const {
    for (const auto& entry : values) {
      if (entry.first == name) {
        return entry.second;
      }
    }
    return std::nullopt;
  }
  [[nodiscard]] std::string get_or(const std::string& name, const std::string& fallback) const {
    const auto found = get(name);
    return found.has_value() ? *found : fallback;
  }
  [[nodiscard]] bool has(const std::string& name) const { return get(name).has_value(); }
  [[nodiscard]] std::vector<std::string> get_all(const std::string& name) const {
    std::vector<std::string> out;
    for (const auto& entry : values) {
      if (entry.first == name) {
        out.push_back(entry.second);
      }
    }
    return out;
  }
};

void print_usage(std::FILE* stream) {
  std::fputs(
      "Queue Observatory -- observation and historical interpretation of network queue state\n"
      "\n"
      "usage: qobs <command> [options]\n"
      "\n"
      "commands:\n"
      "  ingest     decode and apply observation documents\n"
      "  inspect    list tracked queues with their current state\n"
      "  history    show the bounded history and an aggregated window of one queue\n"
      "  pressure   show the deterministic pressure classification of every queue\n"
      "  explain    show the full decision trace that produced one queue state\n"
      "  export     write a deterministic machine-readable export\n"
      "  status     show runtime counters and persistence statistics\n"
      "  recover    re-read the newest persistence segment and report what was usable\n"
      "  events     show the bounded event log\n"
      "  sources    show the source registry and its fencing counters\n"
      "  conflicts  show recorded peer disagreements\n"
      "  burst      show observed microburst evidence\n"
      "  contend    show sibling contention correlation\n"
      "  version    show build information\n"
      "\n"
      "common options:\n"
      "  --file <path>            document to ingest (repeatable)\n"
      "  --persist-dir <path>     enable persistence in this directory\n"
      "  --no-recover             do not recover persisted evidence on start\n"
      "  --persist-on-stop        write a snapshot when the run ends\n"
      "  --workers <n>            background applier threads (default 0: synchronous)\n"
      "  --device <name>          filter by device\n"
      "  --port <name>            filter by port\n"
      "  --queue <index>          filter by queue index\n"
      "  --class <name>           filter by scheduling class\n"
      "  --traffic-class <n>      filter by traffic class\n"
      "  --state <name>           filter by pressure state\n"
      "  --limit <n>              page size (default 100)\n"
      "  --offset <n>             page offset\n"
      "  --window-name <name>     window label (default 1s)\n"
      "  --window-ns <n>          window duration in nanoseconds (default 1000000000)\n"
      "  --bucket-ns <n>          window bucket in nanoseconds (default 125000000)\n"
      "  --min-buckets <n>        covered buckets required for a claim (default 1)\n"
      "  --trace                  include the full decision trace\n"
      "  --json                   emit JSON where the command supports it\n",
      stream);
}

/// Options that never take a value. Without this list a flag followed by the
/// command name would swallow the command as its argument.
bool is_boolean_flag(const std::string& name) {
  static const char* const kFlags[] = {"no-recover",  "persist-on-stop", "trace",
                                       "json",        "no-header",       "summary-only",
                                       "history",     "sufficient-only", "help"};
  for (const char* flag : kFlags) {
    if (name == flag) {
      return true;
    }
  }
  return false;
}

bool parse_options(int argc, char** argv, Options& options, std::string& error) {
  if (argc < 2) {
    error = "a command is required";
    return false;
  }
  // The command is the first token that is not an option, so options may appear
  // before or after it.
  for (int index = 1; index < argc; ++index) {
    const std::string token = argv[index];
    const bool is_option = token.size() > 2u && token[0] == '-' && token[1] == '-';
    if (!is_option) {
      if (options.command.empty()) {
        options.command = token;
      } else {
        options.positional.push_back(token);
      }
      continue;
    }
    const std::string name = token.substr(2);
    const std::size_t equals = name.find('=');
    if (equals != std::string::npos) {
      options.values.emplace_back(name.substr(0, equals), name.substr(equals + 1u));
      continue;
    }
    if (is_boolean_flag(name)) {
      options.values.emplace_back(name, std::string{});
      continue;
    }
    if (index + 1 >= argc) {
      error = "option --" + name + " requires a value";
      return false;
    }
    options.values.emplace_back(name, argv[index + 1]);
    ++index;
  }
  if (options.command.empty()) {
    error = "a command is required";
    return false;
  }
  return true;
}

std::optional<std::uint64_t> parse_u64_option(const Options& options, const std::string& name,
                                              std::string& error) {
  const auto text = options.get(name);
  if (!text.has_value()) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  if (!qobs::text::parse_u64(*text, value)) {
    error = "option --" + name + " requires a non-negative integer";
    return std::nullopt;
  }
  return value;
}

std::optional<std::size_t> parse_size_option(const Options& options, const std::string& name,
                                             std::string& error, bool& failed) {
  failed = false;
  const auto text = options.get(name);
  if (!text.has_value()) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  if (!qobs::text::parse_u64(*text, value) ||
      value > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
    error = "option --" + name + " requires a non-negative integer";
    failed = true;
    return std::nullopt;
  }
  return static_cast<std::size_t>(value);
}

std::optional<qobs::Nanos> parse_nanos_option(const Options& options, const std::string& name,
                                              std::string& error, bool& failed) {
  failed = false;
  const auto text = options.get(name);
  if (!text.has_value()) {
    return std::nullopt;
  }
  std::int64_t value = 0;
  if (!qobs::text::parse_i64(*text, value) || value <= 0) {
    error = "option --" + name + " requires a positive nanosecond duration";
    failed = true;
    return std::nullopt;
  }
  return static_cast<qobs::Nanos>(value);
}

bool build_filter(const Options& options, qobs::QueueFilter& filter, std::string& error) {
  if (const auto device = options.get("device"); device.has_value()) {
    const auto parsed = qobs::DeviceId::create(*device);
    if (!parsed.has_value()) {
      error = "the device filter is not a usable identity";
      return false;
    }
    filter.device = parsed.value();
  }
  if (const auto port = options.get("port"); port.has_value()) {
    const auto parsed = qobs::PortId::create(*port);
    if (!parsed.has_value()) {
      error = "the port filter is not a usable identity";
      return false;
    }
    filter.port = parsed.value();
  }
  if (options.has("queue")) {
    bool failed = false;
    const auto value = parse_size_option(options, "queue", error, failed);
    if (failed) {
      return false;
    }
    if (value.has_value()) {
      const auto narrow = qobs::checked::narrow_u<std::uint32_t>(*value);
      if (!narrow.has_value()) {
        error = "the queue filter does not fit in 32 bits";
        return false;
      }
      filter.queue = qobs::QueueId::from_raw(*narrow);
    }
  }
  if (const auto scheduling = options.get("class"); scheduling.has_value()) {
    const auto parsed = qobs::SchedulingClassId::create(*scheduling);
    if (!parsed.has_value()) {
      error = "the class filter is not a usable identity";
      return false;
    }
    filter.scheduling_class = parsed.value();
  }
  if (options.has("traffic-class")) {
    bool failed = false;
    const auto value = parse_size_option(options, "traffic-class", error, failed);
    if (failed) {
      return false;
    }
    if (value.has_value()) {
      const auto narrow = qobs::checked::narrow_u<std::uint16_t>(*value);
      if (!narrow.has_value()) {
        error = "the traffic class filter does not fit in 16 bits";
        return false;
      }
      filter.traffic_class = qobs::TrafficClassId::from_raw(*narrow);
    }
  }
  if (const auto state = options.get("state"); state.has_value()) {
    const auto parsed = qobs::parse_pressure_state(*state);
    if (!parsed.has_value()) {
      error = "unknown pressure state '" + *state + "'";
      return false;
    }
    filter.state = *parsed;
  }
  return true;
}

bool build_pagination(const Options& options, qobs::QueryPagination& page, std::string& error) {
  bool failed = false;
  if (const auto limit = parse_size_option(options, "limit", error, failed); limit.has_value()) {
    page.limit = *limit;
  }
  if (failed) {
    return false;
  }
  if (const auto offset = parse_size_option(options, "offset", error, failed); offset.has_value()) {
    page.offset = *offset;
  }
  return !failed;
}

bool build_window(const Options& options, qobs::WindowSpec& window, std::string& error) {
  bool failed = false;
  window.name = options.get_or("window-name", "1s");
  if (const auto duration = parse_nanos_option(options, "window-ns", error, failed);
      duration.has_value()) {
    window.duration_ns = *duration;
  }
  if (failed) {
    return false;
  }
  if (const auto bucket = parse_nanos_option(options, "bucket-ns", error, failed);
      bucket.has_value()) {
    window.bucket_ns = *bucket;
  }
  if (failed) {
    return false;
  }
  if (const auto min_buckets = parse_size_option(options, "min-buckets", error, failed);
      min_buckets.has_value()) {
    if (*min_buckets == 0u || *min_buckets > 0xFFFFFFFFull) {
      error = "option --min-buckets must be between 1 and 4294967295";
      return false;
    }
    window.min_covered_buckets = static_cast<std::uint32_t>(*min_buckets);
  }
  if (failed) {
    return false;
  }
  window.max_buckets = 4096u;
  const qobs::Status status = qobs::validate_window_spec(window);
  if (!status.ok()) {
    error = status.to_string();
    return false;
  }
  return true;
}

int fail(const std::string& message) {
  std::fprintf(stderr, "qobs: %s\n", message.c_str());
  return 2;
}

int fail_status(const qobs::Status& status) {
  std::fprintf(stderr, "qobs: %s\n", status.to_string().c_str());
  return 1;
}

std::optional<qobs::QueuePath> parse_queue_path(const Options& options, std::string& error) {
  const auto text = options.get("queue-path");
  if (!text.has_value()) {
    error = "this command requires --queue-path device|port|queue";
    return std::nullopt;
  }
  const auto parsed = qobs::QueuePath::parse(*text);
  if (!parsed.has_value()) {
    error = parsed.error().message();
    return std::nullopt;
  }
  return parsed.value();
}

int run_ingest(qobs::Observatory& runtime, const Options& options) {
  const std::vector<std::string> files = options.get_all("file");
  if (files.empty()) {
    return fail("ingest requires at least one --file");
  }
  std::size_t total_accepted = 0;
  std::size_t total_rejected = 0;
  std::size_t total_malformed = 0;
  for (const std::string& path : files) {
    std::string document;
    const qobs::Status read = qobs::read_document_file(path, runtime.config().ingest.max_payload_bytes,
                                                       document);
    if (!read.ok()) {
      return fail_status(read);
    }
    qobs::IngestReport report;
    const qobs::Status status = runtime.ingest_document(document, report);
    if (!status.ok()) {
      return fail_status(status);
    }
    total_accepted += report.admission.accepted;
    total_rejected += report.admission.rejected;
    total_malformed += report.decode.malformed;
    std::fputs(qobs::render_ingest_report(report).c_str(), stdout);
  }
  std::printf("total_accepted=%zu total_rejected=%zu total_malformed=%zu\n", total_accepted,
              total_rejected, total_malformed);
  return total_rejected == 0 && total_malformed == 0 ? 0 : 3;
}

int run_inspect(qobs::Observatory& runtime, const Options& options) {
  qobs::InspectQuery query;
  std::string error;
  if (!build_filter(options, query.filter, error) || !build_pagination(options, query.page, error)) {
    return fail(error);
  }
  qobs::InspectResult result;
  const qobs::Status status = runtime.inspect(query, result);
  if (!status.ok()) {
    return fail_status(status);
  }
  std::fputs(qobs::render_inspect_table(result, !options.has("no-header")).c_str(), stdout);
  return 0;
}

int run_history(qobs::Observatory& runtime, const Options& options) {
  std::string error;
  const auto queue = parse_queue_path(options, error);
  if (!queue.has_value()) {
    return fail(error);
  }
  qobs::HistoryQuery query;
  query.queue = *queue;
  query.include_records = !options.has("summary-only");
  if (!build_window(options, query.window, error) || !build_pagination(options, query.page, error)) {
    return fail(error);
  }
  qobs::HistoryResult result;
  const qobs::Status status = runtime.history(query, result);
  if (!status.ok()) {
    return fail_status(status);
  }
  std::fputs(qobs::render_history_report(result).c_str(), stdout);
  return result.found ? 0 : 4;
}

int run_pressure(qobs::Observatory& runtime, const Options& options) {
  qobs::PressureQuery query;
  query.include_trace = options.has("trace");
  std::string error;
  if (!build_filter(options, query.filter, error) || !build_pagination(options, query.page, error)) {
    return fail(error);
  }
  qobs::PressureResult result;
  const qobs::Status status = runtime.pressure(query, result);
  if (!status.ok()) {
    return fail_status(status);
  }
  std::fputs(qobs::render_pressure_table(result, !options.has("no-header")).c_str(), stdout);
  if (query.include_trace) {
    for (const qobs::PressureRow& row : result.rows) {
      std::printf("trace %s\n", row.queue.to_string().c_str());
      for (const qobs::TraceEntry& entry : row.trace) {
        std::printf("  rule=%s matched=%s operand=%llu threshold=%llu detail=%s\n",
                    std::string(qobs::rule_name(entry.rule)).c_str(),
                    entry.matched ? "true" : "false",
                    static_cast<unsigned long long>(entry.operand),
                    static_cast<unsigned long long>(entry.threshold), entry.detail.c_str());
      }
    }
  }
  return 0;
}

int run_explain(qobs::Observatory& runtime, const Options& options) {
  std::string error;
  const auto queue = parse_queue_path(options, error);
  if (!queue.has_value()) {
    return fail(error);
  }
  qobs::ExplainQuery query;
  query.queue = *queue;
  qobs::ExplainResult result;
  const qobs::Status status = runtime.explain(query, result);
  if (!status.ok()) {
    return fail_status(status);
  }
  if (!result.found) {
    std::printf("queue=%s found=false\n", query.queue.to_string().c_str());
    return 4;
  }
  std::fputs(result.explanation.c_str(), stdout);
  std::printf("explanation_digest=%llu\n",
              static_cast<unsigned long long>(result.classification.explanation_digest));
  return 0;
}

int run_export(qobs::Observatory& runtime, const Options& options) {
  qobs::ExportQuery query;
  std::string error;
  if (!build_filter(options, query.filter, error) || !build_pagination(options, query.page, error)) {
    return fail(error);
  }
  const std::string format = options.get_or("format", "ndjson");
  const auto parsed_format = qobs::parse_export_format(format);
  if (!parsed_format.has_value()) {
    return fail("unknown export format '" + format + "'");
  }
  query.format = *parsed_format;
  query.include_history = options.has("history");
  if (query.include_history && !build_window(options, query.window, error)) {
    return fail(error);
  }
  qobs::ExportResult result;
  const qobs::Status status = runtime.export_data(query, result);
  if (!status.ok()) {
    return fail_status(status);
  }
  std::fputs(result.document.c_str(), stdout);
  std::fprintf(stderr, "rows=%zu history_records=%zu truncated=%s\n", result.rows,
               result.history_records, result.truncated ? "true" : "false");
  return 0;
}

int run_status(qobs::Observatory& runtime) {
  std::fputs(qobs::render_runtime_status(runtime.status()).c_str(), stdout);
  return 0;
}

int run_recover(qobs::Observatory& runtime) {
  qobs::RecoverySummary summary;
  const qobs::Status status = runtime.recover(summary);
  if (!status.ok()) {
    return fail_status(status);
  }
  std::fputs(qobs::render_recovery_summary(summary).c_str(), stdout);
  return summary.recovered ? 0 : 4;
}

int run_events(qobs::Observatory& runtime, const Options& options) {
  qobs::EventQuery query;
  std::string error;
  if (const auto kind = options.get("kind"); kind.has_value()) {
    bool matched = false;
    for (std::size_t index = 0; index < qobs::kEventKindCount; ++index) {
      const auto candidate = static_cast<qobs::EventKind>(index);
      if (qobs::to_string(candidate) == *kind) {
        query.kind = candidate;
        matched = true;
        break;
      }
    }
    if (!matched) {
      return fail("unknown event kind '" + *kind + "'");
    }
  }
  if (const auto severity = options.get("min-severity"); severity.has_value()) {
    bool matched = false;
    for (const qobs::Severity candidate : {qobs::Severity::Debug, qobs::Severity::Info,
                                           qobs::Severity::Notice, qobs::Severity::Warning,
                                           qobs::Severity::Error}) {
      if (qobs::to_string(candidate) == *severity) {
        query.min_severity = candidate;
        matched = true;
        break;
      }
    }
    if (!matched) {
      return fail("unknown severity '" + *severity + "'");
    }
  }
  if (!build_pagination(options, query.page, error)) {
    return fail(error);
  }
  qobs::EventResult result;
  const qobs::Status status = runtime.events(query, result);
  if (!status.ok()) {
    return fail_status(status);
  }
  std::fputs(qobs::render_events_report(result).c_str(), stdout);
  return 0;
}

int run_sources(qobs::Observatory& runtime) {
  std::vector<qobs::SourceRecord> sources;
  const qobs::Status status = runtime.sources(sources);
  if (!status.ok()) {
    return fail_status(status);
  }
  std::fputs(qobs::render_sources_report(sources).c_str(), stdout);
  return 0;
}

int run_conflicts(qobs::Observatory& runtime) {
  std::vector<qobs::ConflictRecord> conflicts;
  const qobs::Status status = runtime.conflicts(conflicts);
  if (!status.ok()) {
    return fail_status(status);
  }
  std::fputs(qobs::render_conflicts_report(conflicts).c_str(), stdout);
  return 0;
}

int run_burst(qobs::Observatory& runtime, const Options& options) {
  qobs::MicroburstQuery query;
  std::string error;
  if (!build_filter(options, query.filter, error) || !build_pagination(options, query.page, error)) {
    return fail(error);
  }
  query.include_insufficient = !options.has("sufficient-only");
  qobs::MicroburstResult result;
  const qobs::Status status = runtime.microburst(query, result);
  if (!status.ok()) {
    return fail_status(status);
  }
  std::fputs(qobs::render_microburst_report(result).c_str(), stdout);
  return 0;
}

int run_contend(qobs::Observatory& runtime, const Options& options) {
  qobs::ContentionQuery query;
  std::string error;
  if (options.has("device") || options.has("port")) {
    qobs::PortPath path;
    const auto device = options.get("device");
    const auto port = options.get("port");
    if (!device.has_value() || !port.has_value()) {
      return fail("--device and --port must be supplied together for a contention query");
    }
    const auto parsed_device = qobs::DeviceId::create(*device);
    const auto parsed_port = qobs::PortId::create(*port);
    if (!parsed_device.has_value() || !parsed_port.has_value()) {
      return fail("the port selector is not a usable identity");
    }
    path.device = parsed_device.value();
    path.port = parsed_port.value();
    query.port = path;
  }
  if (const auto scheduling = options.get("class"); scheduling.has_value()) {
    const auto parsed = qobs::SchedulingClassId::create(*scheduling);
    if (!parsed.has_value()) {
      return fail("the class filter is not a usable identity");
    }
    query.scheduling_class = parsed.value();
  }
  if (!build_pagination(options, query.page, error)) {
    return fail(error);
  }
  qobs::ContentionResult result;
  const qobs::Status status = runtime.contention(query, result);
  if (!status.ok()) {
    return fail_status(status);
  }
  std::fputs(qobs::render_contention_report(result).c_str(), stdout);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  std::string error;
  if (!parse_options(argc, argv, options, error)) {
    print_usage(stderr);
    return fail(error);
  }
  if (options.command == "help" || options.command == "--help" || options.command == "-h") {
    print_usage(stdout);
    return 0;
  }
  if (options.command == "version") {
    std::printf("qobs %s compiler=%s configuration=%s sanitizer=%s lock_audit=%s\n",
                std::string(qobs::version_string()).c_str(),
                std::string(qobs::build_compiler()).c_str(),
                std::string(qobs::build_configuration()).c_str(),
                qobs::build_has_address_sanitizer() ? "yes" : "no",
                qobs::build_has_lock_audit() ? "enabled" : "disabled");
    std::printf("persistence_format=%u wire_format=%u policy=%u\n",
                QOBS_PERSISTENCE_FORMAT_VERSION, QOBS_WIRE_FORMAT_VERSION, QOBS_POLICY_VERSION);
    return 0;
  }

  qobs::ObservatoryConfig config;
  // The command line applies documents synchronously unless a caller asks for
  // background workers, so that every subcommand observes a settled runtime.
  config.runtime.worker_threads = 0;
  bool failed = false;
  if (options.has("workers")) {
    const auto workers = parse_size_option(options, "workers", error, failed);
    if (failed) {
      return fail(error);
    }
    if (workers.has_value()) {
      config.runtime.worker_threads = *workers;
    }
  }
  if (const auto directory = options.get("persist-dir"); directory.has_value()) {
    config.persistence_directory = std::filesystem::path(*directory);
  }
  config.recover_on_start = !options.has("no-recover");
  config.persist_on_stop = options.has("persist-on-stop");

  auto runtime = qobs::Observatory::create(config, nullptr);
  if (!runtime.has_value()) {
    return fail_status(qobs::Status(runtime.error()));
  }
  std::unique_ptr<qobs::Observatory> observatory = std::move(runtime).value();
  const qobs::Status started = observatory->start();
  if (!started.ok()) {
    return fail_status(started);
  }

  int exit_code = 0;
  const std::string& command = options.command;
  if (command == "ingest") {
    exit_code = run_ingest(*observatory, options);
  } else if (command == "inspect") {
    exit_code = run_inspect(*observatory, options);
  } else if (command == "history") {
    exit_code = run_history(*observatory, options);
  } else if (command == "pressure") {
    exit_code = run_pressure(*observatory, options);
  } else if (command == "explain") {
    exit_code = run_explain(*observatory, options);
  } else if (command == "export") {
    exit_code = run_export(*observatory, options);
  } else if (command == "status") {
    exit_code = run_status(*observatory);
  } else if (command == "recover") {
    exit_code = run_recover(*observatory);
  } else if (command == "events") {
    exit_code = run_events(*observatory, options);
  } else if (command == "sources") {
    exit_code = run_sources(*observatory);
  } else if (command == "conflicts") {
    exit_code = run_conflicts(*observatory);
  } else if (command == "burst") {
    exit_code = run_burst(*observatory, options);
  } else if (command == "contend") {
    exit_code = run_contend(*observatory, options);
  } else {
    print_usage(stderr);
    exit_code = fail("unknown command '" + command + "'");
  }

  const qobs::Status stopped = observatory->stop();
  if (!stopped.ok() && exit_code == 0) {
    return fail_status(stopped);
  }
  return exit_code;
}
