#pragma once

#include <cstdint>
#include <string>

#include "qobs/core/Result.hpp"
#include "qobs/core/StrongId.hpp"

namespace qobs {

using Nanos = std::int64_t;

/// Nanoseconds since an unspecified steady origin. Only differences are
/// meaningful; the origin is process-local.
struct SteadyTime {
  Nanos ns{0};
  friend constexpr bool operator==(SteadyTime, SteadyTime) = default;
  friend constexpr auto operator<=>(SteadyTime, SteadyTime) = default;
};

/// Nanoseconds since the Unix epoch as reported by the host wall clock. The
/// wall clock can move backwards; it is recorded for human correlation only and
/// is never used for ordering or freshness decisions.
struct WallTime {
  Nanos ns{0};
  friend constexpr bool operator==(WallTime, WallTime) = default;
  friend constexpr auto operator<=>(WallTime, WallTime) = default;
};

/// The pair recorded for every piece of evidence that enters the runtime.
struct ReceiveTime {
  SteadyTime steady{};
  WallTime wall{};
};

/// A source-reported timestamp together with the clock domain that produced it.
///
/// Timestamps from different clock domains are never compared for ordering,
/// gap, or freshness; the domain travels with the value precisely so that a
/// comparison can be refused instead of silently producing a wrong delta.
struct ObservationTime {
  Nanos ns{0};
  ClockDomainId domain{};

  [[nodiscard]] bool known() const noexcept { return domain.valid(); }
  friend bool operator==(const ObservationTime& lhs, const ObservationTime& rhs) noexcept {
    return lhs.ns == rhs.ns && lhs.domain == rhs.domain;
  }
};

enum class ClockComparability : std::uint8_t {
  Comparable = 0,
  DifferentDomains,
  UnknownDomain,
};

[[nodiscard]] std::string_view to_string(ClockComparability value) noexcept;

/// Result of comparing two observation times, carrying the reason when the
/// comparison is refused.
struct TimeComparison {
  ClockComparability comparability{ClockComparability::UnknownDomain};
  /// Only meaningful when comparability == Comparable: -1, 0 or 1.
  int ordering{0};

  [[nodiscard]] bool comparable() const noexcept {
    return comparability == ClockComparability::Comparable;
  }
};

[[nodiscard]] TimeComparison compare_observation_times(const ObservationTime& lhs,
                                                       const ObservationTime& rhs) noexcept;

/// Clock abstraction. Production uses SystemClock; deterministic tests inject
/// ManualClock so that every freshness and window decision is reproducible.
class Clock {
 public:
  Clock() = default;
  virtual ~Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;

  [[nodiscard]] virtual ReceiveTime now() const = 0;
};

class SystemClock final : public Clock {
 public:
  SystemClock() = default;
  [[nodiscard]] ReceiveTime now() const override;
};

/// A clock that only moves when a caller moves it.
class ManualClock final : public Clock {
 public:
  ManualClock() = default;
  explicit ManualClock(SteadyTime start);

  [[nodiscard]] ReceiveTime now() const override;
  void advance(Nanos delta) noexcept;
  void set_steady(SteadyTime value) noexcept;
  void set_wall(WallTime value) noexcept;

 private:
  SteadyTime steady_{};
  WallTime wall_{};
};

[[nodiscard]] SteadyTime steady_now() noexcept;
[[nodiscard]] WallTime wall_now() noexcept;
[[nodiscard]] ReceiveTime receive_now() noexcept;

/// Format helpers. Wall time is rendered as UTC ISO-8601 with nanosecond
/// precision; steady time is rendered with an explicit process-relative marker
/// so that it can never be mistaken for a wall timestamp.
[[nodiscard]] std::string format_wall_utc(WallTime value);
[[nodiscard]] std::string format_steady(SteadyTime value);
[[nodiscard]] Result<WallTime> parse_wall_utc(std::string_view text);
[[nodiscard]] Result<SteadyTime> parse_steady(std::string_view text);

}  // namespace qobs
