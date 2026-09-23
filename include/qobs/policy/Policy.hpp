#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "qobs/core/Result.hpp"
#include "qobs/core/Time.hpp"
#include "qobs/model/Counter.hpp"
#include "qobs/model/Evidence.hpp"
#include "qobs/model/Sample.hpp"
#include "qobs/version.hpp"

namespace qobs {

/// The ten states a queue may be reported in. Every one of them is reachable
/// and every one of them is produced by an explicit, ordered rule.
enum class PressureState : std::uint8_t {
  Unknown = 0,
  Idle,
  Normal,
  Elevated,
  Pressured,
  Saturated,
  Dropping,
  Paused,
  Stale,
  Conflicting,
};

inline constexpr std::size_t kPressureStateCount = 10;

[[nodiscard]] std::string_view to_string(PressureState state) noexcept;
[[nodiscard]] std::optional<PressureState> parse_pressure_state(std::string_view text) noexcept;

/// Severity ordering used for stable report ordering and for transition
/// detection. It is deliberately separate from the enumeration values so that
/// reordering the enum cannot silently reorder reports.
[[nodiscard]] std::uint32_t pressure_severity_rank(PressureState state) noexcept;

/// Which measurement the threshold comparison used.
enum class PressureBasisKind : std::uint8_t {
  /// No usable basis: required measurements were absent.
  None = 0,
  /// Occupancy expressed in buffer cells compared against a cell threshold.
  CellsAgainstThreshold,
  /// Queue depth in packets compared against the configured maximum depth.
  PacketsAgainstDepth,
  /// Occupancy compared against an absolute value configured in the policy.
  Absolute,
};

[[nodiscard]] std::string_view to_string(PressureBasisKind kind) noexcept;

struct PressureThresholds {
  /// Absolute thresholds in the unit of the chosen basis. When the basis is
  /// relative these are unused.
  std::optional<std::uint64_t> elevated_at{};
  std::optional<std::uint64_t> pressured_at{};
  std::optional<std::uint64_t> saturated_at{};
  /// Drop count inside the evaluation window that means "dropping".
  std::uint64_t dropping_at_drops{1};
  /// Pause evidence inside the evaluation window that means "paused".
  std::uint64_t paused_at_pause_frames{1};
  std::uint64_t paused_at_pause_nanos{1};
  /// Mark (ECN) count inside the evaluation window that counts as a pressure
  /// indicator on its own.
  std::uint64_t marks_indicate_pressure_at{1};

  friend bool operator==(const PressureThresholds&, const PressureThresholds&) = default;
};

/// Relative thresholds expressed in per-mille of the applicable limit, so the
/// comparison is pure integer arithmetic and identical on every platform.
struct RelativeThresholds {
  std::uint32_t elevated_permille{500};
  std::uint32_t pressured_permille{750};
  std::uint32_t saturated_permille{950};

  friend bool operator==(const RelativeThresholds&, const RelativeThresholds&) = default;
};

/// Microburst evidence policy.
///
/// Queue Observatory only ever reports bursts that were *observed* inside the
/// sampling grid. It never reconstructs behaviour between samples; a window
/// whose bucket coverage is below the required minimum is reported as
/// insufficient coverage instead of being extrapolated.
struct MicroburstPolicy {
  Nanos window_ns{1000000000LL};
  Nanos bucket_ns{10000000LL};
  /// Minimal peak-to-trough excursion inside the window.
  std::uint64_t min_excursion{1};
  /// Minimum number of distinct buckets that must contain at least one sample.
  std::uint32_t min_covered_buckets{3};
  /// Upper bound on buckets per window; the window is rejected when it would
  /// need more.
  std::size_t max_buckets{1024};
  /// When true, an under-covered window produces an explicit
  /// insufficient-coverage record instead of silence.
  bool report_insufficient_coverage{true};

  friend bool operator==(const MicroburstPolicy&, const MicroburstPolicy&) = default;
};

/// Sibling contention policy.
struct ContentionPolicy {
  Nanos window_ns{5000000000LL};
  /// Minimum number of buckets that must be covered before any correlation is
  /// claimed.
  std::uint32_t min_covered_buckets{4};
  /// Minimum number of buckets in which both siblings were at or above the
  /// contention state before contention is reported.
  std::uint32_t min_simultaneous_buckets{1};
  /// States that count as contention for membership tests.
  bool count_pressured{true};
  bool count_saturated{true};
  bool count_dropping{true};
  bool count_paused{true};

  friend bool operator==(const ContentionPolicy&, const ContentionPolicy&) = default;
};

/// Policy for deciding when two equally authoritative sources disagree.
struct ConflictPolicy {
  bool enabled{true};
  /// Two reports further apart in receive time than this are not compared.
  Nanos window_ns{5000000000LL};
  /// Absolute tolerance in the unit of the compared measurement.
  std::uint64_t absolute_tolerance{0};
  /// Relative tolerance in per-mille of the larger of the two values.
  std::uint32_t relative_permille{100};

  friend bool operator==(const ConflictPolicy&, const ConflictPolicy&) = default;
};

/// The complete, versioned classification policy.
struct PressurePolicy {
  std::uint32_t version{QOBS_POLICY_VERSION};
  std::string name{"default"};

  FreshnessPolicy freshness{};
  ContinuityPolicy continuity{};

  /// Fields that must all be reported for a classification to be complete.
  FieldMask required_fields{0};
  /// Fields of which at least one must be reported. The default accepts either
  /// unit of occupancy, because a source that reports bytes and a source that
  /// reports cells are both reporting the same physical quantity.
  FieldMask required_any_fields{field_bit(SampleField::OccupancyCells) |
                                field_bit(SampleField::OccupancyBytes) |
                                field_bit(SampleField::QueueDepthPackets)};

  /// Resolution order for the pressure basis, most preferred first.
  bool prefer_cells_over_packets{true};
  /// When true, an absolute threshold from PressureThresholds is used when no
  /// relative limit is available.
  bool allow_absolute_thresholds{true};
  /// When true, thresholds derived from OccupancyBytes are allowed.
  bool allow_byte_basis{false};

  PressureThresholds absolute{};
  RelativeThresholds relative{};

  /// Window over which counter deltas (drops, marks, pauses) are aggregated
  /// before they can drive a state.
  Nanos evaluation_window_ns{1000000000LL};

  ConflictPolicy conflict{};
  MicroburstPolicy microburst{};
  ContentionPolicy contention{};

  friend bool operator==(const PressurePolicy&, const PressurePolicy&) = default;
};

/// The policy every runtime uses unless a caller supplies another one.
[[nodiscard]] const PressurePolicy& default_pressure_policy();

/// A policy is only accepted when it is internally coherent; an incoherent
/// policy is rejected at construction rather than producing a surprise state at
/// runtime.
[[nodiscard]] Status validate_policy(const PressurePolicy& policy);

/// Compare two thresholds without ever mixing units: the caller supplies the
/// limit and the value in the same unit.
[[nodiscard]] Result<std::uint64_t> permille_of(std::uint64_t limit,
                                                std::uint32_t permille) noexcept;

}  // namespace qobs
