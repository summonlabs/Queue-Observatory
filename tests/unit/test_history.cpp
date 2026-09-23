#include "support/TestHarness.hpp"

#include "qobs/store/History.hpp"

using namespace qobs;

namespace {

HistoryRecord record(Nanos at, std::uint64_t occupancy) {
  HistoryRecord value;
  value.received.steady = SteadyTime{at};
  value.received.wall = WallTime{at};
  value.observed.ns = at;
  value.observed.domain = ClockDomainId(std::string("d"));
  value.source = SourceId(std::string("s"));
  value.generation = GenerationId::from_raw(1);
  value.sequence = SourceSequence::from_raw(static_cast<std::uint64_t>(at) + 1u);
  value.authority = SourceAuthority::Primary;
  value.reported = field_bit(SampleField::OccupancyCells) | field_bit(SampleField::DropPackets);
  value.freshness = Freshness::Fresh;
  value.quality = EvidenceQuality::Complete;
  value.values[static_cast<std::size_t>(SampleField::OccupancyCells)] = occupancy;
  value.deltas[static_cast<std::size_t>(SampleField::DropPackets)].known = true;
  value.deltas[static_cast<std::size_t>(SampleField::DropPackets)].delta = 1;
  return value;
}

}  // namespace

QOBS_TEST(history, ring_keeps_the_newest_records) {
  HistoryRing ring;
  ring.reserve(3);
  QOBS_CHECK_EQ(ring.capacity(), 3u);
  for (Nanos index = 0; index < 5; ++index) {
    ring.push(record(index, static_cast<std::uint64_t>(index)));
  }
  QOBS_CHECK_EQ(ring.size(), 3u);
  QOBS_CHECK_EQ(ring.evicted(), 2u);
  QOBS_CHECK_EQ(ring.at(0).received.steady.ns, 2);
  QOBS_CHECK_EQ(ring.newest().received.steady.ns, 4);
  QOBS_CHECK_EQ(ring.bytes(), 3u * sizeof(HistoryRecord));
}

QOBS_TEST(history, ring_with_no_capacity_counts_rejections) {
  HistoryRing ring;
  ring.reserve(0);
  ring.push(record(1, 1));
  QOBS_CHECK_EQ(ring.size(), 0u);
  QOBS_CHECK_EQ(ring.evicted(), 1u);
  QOBS_CHECK(!ring.newest().establishes_current());
}

QOBS_TEST(history, snapshot_and_since_are_ordered) {
  HistoryRing ring;
  ring.reserve(8);
  for (Nanos index = 0; index < 6; ++index) {
    ring.push(record(index * 100, static_cast<std::uint64_t>(index)));
  }
  const std::vector<HistoryRecord> all = ring.snapshot();
  QOBS_CHECK_EQ(all.size(), 6u);
  QOBS_CHECK_EQ(QOBS_FRONT(all).received.steady.ns, 0);
  QOBS_CHECK_EQ(QOBS_BACK(all).received.steady.ns, 500);

  const std::vector<HistoryRecord> recent = ring.since(300);
  QOBS_CHECK_EQ(recent.size(), 3u);
  QOBS_CHECK_EQ(QOBS_FRONT(recent).received.steady.ns, 300);
  QOBS_CHECK_EQ(QOBS_BACK(recent).received.steady.ns, 500);
}

QOBS_TEST(history, missing_fields_are_absent_and_deltas_default_to_unknown) {
  const HistoryRecord value = record(10, 7);
  QOBS_CHECK_EQ(value.value(SampleField::OccupancyCells).value(), 7u);
  QOBS_CHECK(!value.value(SampleField::OccupancyBytes).has_value());
  QOBS_CHECK(value.delta(SampleField::DropPackets).known);
  QOBS_CHECK(!value.delta(SampleField::MarkPackets).known);
  QOBS_CHECK(value.establishes_current());
}

QOBS_TEST(history, stale_records_do_not_speak_for_the_present) {
  HistoryRecord value = record(10, 7);
  value.freshness = Freshness::Stale;
  QOBS_CHECK(!value.establishes_current());
  value.freshness = Freshness::Fresh;
  value.quality = EvidenceQuality::Incomplete;
  QOBS_CHECK(!value.establishes_current());
  value.quality = EvidenceQuality::Complete;
  value.authority = SourceAuthority::Secondary;
  QOBS_CHECK(!value.establishes_current());
}
