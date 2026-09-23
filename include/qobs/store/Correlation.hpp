#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "qobs/core/Time.hpp"
#include "qobs/model/Evidence.hpp"
#include "qobs/model/Identity.hpp"
#include "qobs/store/Query.hpp"
#include "qobs/store/Window.hpp"

namespace qobs {

/// Evidence that two queues on the same port competed for the same resource.
///
/// Correlation is only ever claimed from buckets in which both siblings were
/// observed. The ratio is kept as an exact rational so the report is identical
/// on every platform.
struct ContentionRecord {
  PortPath port{};
  SchedulingClassId scheduling_class{};
  QueuePath queue_a{};
  QueuePath queue_b{};
  WindowSpec window{};
  std::size_t covered_buckets{0};
  std::size_t simultaneous_buckets{0};
  std::size_t union_buckets{0};
  std::uint64_t ratio_numerator{0};
  std::uint64_t ratio_denominator{1};
  Nanos window_start_ns{0};
  Nanos window_end_ns{0};
  EvidenceAssessment assessment{};
};

/// Evidence of a short excursion observed inside the sampling grid.
struct MicroburstRecord {
  QueuePath queue{};
  WindowSpec window{};
  std::uint64_t peak{0};
  std::uint64_t trough{0};
  std::uint64_t excursion{0};
  std::uint64_t required_excursion{0};
  std::size_t covered_buckets{0};
  std::size_t required_buckets{0};
  Nanos peak_at_ns{0};
  Nanos trough_at_ns{0};
  bool detected{false};
  bool coverage_sufficient{false};
  /// Always false. Queue Observatory never reconstructs behaviour between
  /// samples; the field is present so a consumer can assert the boundary.
  bool sub_sample_reconstructed{false};
  EvidenceAssessment assessment{};
};

struct ContentionQuery {
  std::optional<PortPath> port{};
  std::optional<SchedulingClassId> scheduling_class{};
  QueryPagination page{};
};

struct ContentionResult {
  std::vector<ContentionRecord> records{};
  std::size_t total_matched{0};
  bool truncated{false};
  /// Ports that were examined and produced no claim, with the reason.
  std::size_t ports_examined{0};
  std::size_t pairs_examined{0};
  std::size_t pairs_insufficient_coverage{0};
};

struct MicroburstQuery {
  QueueFilter filter{};
  QueryPagination page{};
  /// When false, windows without sufficient coverage are omitted instead of
  /// being reported as insufficient-coverage records.
  bool include_insufficient{true};
};

struct MicroburstResult {
  std::vector<MicroburstRecord> records{};
  std::size_t total_matched{0};
  bool truncated{false};
  std::size_t queues_examined{0};
  std::size_t queues_insufficient{0};
};

/// Reduce a fraction to lowest terms. Denominator zero is normalised to one.
void reduce_fraction(std::uint64_t& numerator, std::uint64_t& denominator) noexcept;

}  // namespace qobs
