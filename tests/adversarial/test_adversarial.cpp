#include "support/TestHarness.hpp"

#include <memory>
#include <string>
#include <vector>

#include "qobs/core/Crc32c.hpp"
#include "qobs/core/Json.hpp"
#include "qobs/ingest/Ingest.hpp"
#include "qobs/ingest/Wire.hpp"
#include "qobs/runtime/Observatory.hpp"
#include "qobs/store/Store.hpp"
#include "qobs/transport/Protocol.hpp"

using namespace qobs;
using transport::DecodeFrameResult;
using transport::Frame;
using transport::FrameReader;
using transport::FrameType;
using qobs::test::SampleBuilder;

namespace {

std::string valid_line(std::uint64_t sequence, std::uint32_t queue_index) {
  SampleBuilder builder("leaf-01", "ethernet1/1", queue_index, "collector-a", "boot-1");
  builder.sequence(sequence).received_at(1000).observed_at(1000);
  builder.set(SampleField::OccupancyCells, 10u);
  builder.set(SampleField::DynamicThresholdCells, 1000u);
  std::string line;
  const Status status = encode_sample(builder.build(), line);
  (void)status;
  return line;
}

std::unique_ptr<QueueStore> make_store() {
  auto clock = std::make_shared<ManualClock>(SteadyTime{1000});
  HistoryLimits limits;
  limits.max_queues = 32;
  limits.max_events_total = 1024;
  return std::make_unique<QueueStore>(PressurePolicy{}, limits, QueryLimits{}, clock);
}

}  // namespace

QOBS_TEST(adversarial, hostile_json_documents_are_refused_without_crashing) {
  const std::vector<std::string> hostile{
      "",
      "\n\n\n",
      "{",
      "}",
      "[]",
      "null",
      "\"a string\"",
      "{\"kind\":",
      "{\"kind\":\"queue_sample\",\"v\":1,",
      std::string("{\"kind\":\"queue_sample\",\"v\":1,\"pad\":\"") + std::string(5000u, 'x') + "\"}",
      "{\"kind\":\"queue_sample\",\"v\":1,\"queue\":-1}",
      "{\"kind\":\"queue_sample\",\"v\":1,\"queue\":1.5}",
      "{\"kind\":\"queue_sample\",\"v\":1,\"queue\":99999999999999999999}",
      "{\"kind\":\"queue_sample\",\"v\":1,\"device\":\"a\\u0000b\"}",
      "{\"kind\":\"queue_sample\",\"v\":1,\"device\":\"\\ud800\"}",
      "{\"kind\":\"queue_sample\",\"v\":1,\"device\":\"\\udc00\\ud800\"}",
      "{\"kind\":\"queue_sample\",\"v\":1,\"device\":\"ok\",\"port\":\"p\",\"queue\":0,"
      "\"source\":\"s\",\"incarnation\":\"i\",\"clock_domain\":\"d\",\"generation\":1,"
      "\"sequence\":1,\"observed_ns\":1,\"occupancy_cells\":1,\"occupancy_cells\":2}",
      "{\"kind\":\"queue_sample\",\"v\":1,\"device\":null}",
      "{\"kind\":\"queue_sample\",\"v\":\"1\"}",
      "{\"kind\":\"queue_sample\",\"v\":1,\"authority\":\"root\"}",
      "{\"kind\":\"queue_sample\",\"v\":1,\"traffic_class\":99999}",
      "{\"kind\":\"metadata\",\"v\":1}",
      "{\"kind\":\"class_metadata\",\"v\":1}",
  };
  for (const std::string& document : hostile) {
    DecodeOutcome outcome;
    const Status status = decode_document(document, IngestLimits{}, outcome);
    QOBS_CHECK_STATUS(status);
    QOBS_CHECK(outcome.sample_count == 0u);
    QOBS_CHECK(outcome.metadata_count == 0u);
  }
}

QOBS_TEST(adversarial, absurd_nesting_is_bounded) {
  std::string document = "{\"kind\":\"queue_sample\",\"v\":1,\"deep\":";
  for (int depth = 0; depth < 5000; ++depth) {
    document.push_back('[');
  }
  for (int depth = 0; depth < 5000; ++depth) {
    document.push_back(']');
  }
  document.push_back('}');
  DecodeOutcome outcome;
  QOBS_CHECK_STATUS(decode_document(document, IngestLimits{}, outcome));
  QOBS_CHECK_EQ(outcome.sample_count, 0u);
  QOBS_CHECK(outcome.malformed >= 1u);
}

QOBS_TEST(adversarial, embedded_nul_and_carriage_returns_are_handled) {
  std::string document = valid_line(1, 0);
  document.push_back('\r');
  document.push_back('\n');
  document += valid_line(2, 0);
  document.push_back('\n');
  document.push_back('\0');
  document += valid_line(3, 0);
  IngestReport report;
  auto store = make_store();
  QOBS_CHECK_STATUS(ingest_document(*store, document, IngestLimits{}, report));
  QOBS_CHECK_EQ(report.decode.sample_count, 2u);
  QOBS_CHECK(report.decode.malformed >= 1u);
}

QOBS_TEST(adversarial, duplicate_replay_storm_does_not_grow_state) {
  auto store = make_store();
  const std::string line = valid_line(1, 0);
  IngestReport first;
  QOBS_CHECK_STATUS(ingest_document(*store, line, IngestLimits{}, first));
  QOBS_CHECK_EQ(first.admission.accepted, 1u);
  for (int attempt = 0; attempt < 200; ++attempt) {
    IngestReport replay;
    QOBS_CHECK_STATUS(ingest_document(*store, line, IngestLimits{}, replay));
    QOBS_CHECK_EQ(replay.admission.accepted, 0u);
    QOBS_CHECK_EQ(replay.admission.fenced, 1u);
  }
  QOBS_CHECK_EQ(store->counters().samples_accepted, 1u);
  QOBS_CHECK_EQ(store->counters().duplicates_suppressed, 200u);
  QOBS_CHECK_EQ(store->counters().history_records_pushed, 1u);
  InspectQuery query;
  InspectResult inspected;
  QOBS_CHECK_STATUS(store->inspect(query, inspected));
  QOBS_REQUIRE(inspected.rows.size() == 1u);
  QOBS_CHECK_EQ(QOBS_FRONT(inspected.rows).history_records, 1u);
}

QOBS_TEST(adversarial, queue_flood_is_refused_at_the_budget) {
  auto store = make_store();
  std::size_t rejected = 0;
  for (std::uint32_t index = 0; index < 200u; ++index) {
    IngestReport report;
    // A distinct sequence per document, otherwise every sample after the first
    // would be fenced as a duplicate replay of the same source.
    const Status status =
        ingest_document(*store, valid_line(index + 1u, index), IngestLimits{}, report);
    if (!status.ok()) {
      QOBS_CHECK_EQ(status.code(), ErrorCode::LimitExceeded);
      ++rejected;
      break;
    }
    rejected += report.admission.rejected;
  }
  QOBS_CHECK(rejected > 0u);
  QOBS_CHECK_EQ(store->queue_count(), 32u);
  const StoreCounters counters = store->counters();
  QOBS_CHECK_EQ(counters.samples_presented, 200u);
  QOBS_CHECK_EQ(counters.samples_accepted, 32u);
  QOBS_CHECK_EQ(counters.samples_rejected, 168u);
  QOBS_CHECK_EQ(counters.samples_fenced, 0u);
}

QOBS_TEST(adversarial, clock_regression_never_reports_fresh_evidence) {
  auto clock = std::make_shared<ManualClock>(SteadyTime{1000000});
  HistoryLimits limits;
  limits.max_queues = 4;
  QueueStore store(PressurePolicy{}, limits, QueryLimits{}, clock);
  SampleBuilder builder("leaf-01", "ethernet1/1", 0u, "collector-a", "boot-1");
  builder.sequence(1).received_at(5000000).observed_at(5000000);
  builder.set(SampleField::OccupancyCells, 10u);
  AdmissionResult admission;
  QOBS_CHECK_STATUS(store.ingest(builder.build(), admission));
  QOBS_CHECK(admission.accepted);
  // The sample claims a receive time in the future; the age is negative and the
  // assessment must record the anomaly rather than treat it as fresh.
  QOBS_CHECK(has_flag(admission.classification.assessment.flags, EvidenceFlag::OrderAnomaly));
}

QOBS_TEST(adversarial, frame_decoding_refuses_every_malformed_header) {
  TransportLimits limits;
  std::vector<std::byte> encoded;
  Frame frame;
  frame.type = FrameType::Batch;
  frame.payload = "payload";
  QOBS_CHECK_STATUS(encode_frame(frame, limits, encoded));

  DecodeFrameResult result;
  QOBS_CHECK_STATUS(decode_frame(encoded, limits, result));
  QOBS_CHECK(result.complete);
  QOBS_CHECK_EQ(result.consumed, encoded.size());

  std::vector<std::byte> damaged = encoded;
  damaged[0] = std::byte{0};
  QOBS_CHECK(!decode_frame(damaged, limits, result).ok());

  damaged = encoded;
  damaged[4] = std::byte{9};
  QOBS_CHECK_EQ(decode_frame(damaged, limits, result).code(), ErrorCode::VersionMismatch);

  damaged = encoded;
  damaged[6] = std::byte{42};
  QOBS_CHECK_EQ(decode_frame(damaged, limits, result).code(), ErrorCode::NotSupported);

  damaged = encoded;
  damaged[8] = std::byte{0xFF};
  damaged[9] = std::byte{0xFF};
  damaged[10] = std::byte{0xFF};
  damaged[11] = std::byte{0x7F};
  QOBS_CHECK_EQ(decode_frame(damaged, limits, result).code(), ErrorCode::LimitExceeded);

  damaged = encoded;
  damaged[14] = static_cast<std::byte>(std::to_integer<std::uint8_t>(damaged[14]) ^ 0xFFu);
  QOBS_CHECK_EQ(decode_frame(damaged, limits, result).code(), ErrorCode::IntegrityFailure);

  const std::span<const std::byte> partial(encoded.data(), encoded.size() - 1u);
  QOBS_CHECK_STATUS(decode_frame(partial, limits, result));
  QOBS_CHECK(!result.complete);
}

QOBS_TEST(adversarial, frame_reader_bound_is_enforced) {
  TransportLimits limits;
  limits.max_connection_buffer_bytes = 64u;
  transport::FrameReader reader(limits);
  const std::vector<std::byte> chunk(48u, std::byte{0});
  QOBS_CHECK_STATUS(reader.append(chunk));
  QOBS_CHECK_FAILS(reader.append(chunk));
}

QOBS_TEST(adversarial, hostile_messages_never_reach_the_store) {
  ObservatoryConfig config;
  config.runtime.worker_threads = 0;
  auto created = Observatory::create(config, nullptr);
  QOBS_REQUIRE(created.has_value());
  std::unique_ptr<Observatory> runtime = std::move(created).value();
  QOBS_CHECK_STATUS(runtime->start());
  for (int attempt = 0; attempt < 50; ++attempt) {
    IngestReport report;
    const Status status = runtime->ingest_document("{\"kind\":\"unknown\"}", report);
    QOBS_CHECK_STATUS(status);
    QOBS_CHECK_EQ(report.admission.presented, 0u);
  }
  QOBS_CHECK_EQ(runtime->status().counters.samples_presented, 0u);
  QOBS_CHECK_EQ(runtime->status().queues, 0u);
  QOBS_CHECK_STATUS(runtime->stop());
}

QOBS_TEST(adversarial, a_counter_that_regresses_every_time_stays_bounded) {
  auto store = make_store();
  std::string document;
  for (std::uint64_t sequence = 1; sequence <= 50u; ++sequence) {
    SampleBuilder builder("leaf-01", "ethernet1/1", 0u, "collector-a", "boot-1");
    const Nanos at = 1000 + static_cast<Nanos>(sequence) * 1000;
    builder.sequence(sequence).received_at(at).observed_at(at);
    builder.set(SampleField::OccupancyCells, 10u);
    builder.set(SampleField::DynamicThresholdCells, 1000u);
    // Strictly decreasing and never equal, so every step after the first is a
    // backwards move that is far too small to be a wrap of a 32-bit counter.
    builder.set(SampleField::DropPackets, 1000000u - sequence * 100u);
    std::string line;
    QOBS_CHECK_STATUS(encode_sample(builder.build(), line));
    document += line;
    document.push_back('\n');
  }
  IngestReport report;
  QOBS_CHECK_STATUS(ingest_document(*store, document, IngestLimits{}, report));
  QOBS_CHECK_EQ(report.admission.accepted, 50u);
  // Every backwards step is a reset, and a reset never contributes a delta.
  QOBS_CHECK_EQ(store->counters().counter_resets, 49u);
  const PressureQuery query;
  PressureResult pressure;
  QOBS_CHECK_STATUS(store->pressure(query, pressure));
  QOBS_REQUIRE(pressure.rows.size() == 1u);
  QOBS_CHECK_NE(QOBS_FRONT(pressure.rows).state, PressureState::Dropping);
}
