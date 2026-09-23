#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "qobs/core/Time.hpp"
#include "qobs/model/Counter.hpp"
#include "qobs/model/Evidence.hpp"
#include "qobs/model/Sample.hpp"
#include "qobs/policy/Policy.hpp"

namespace qobs {

/// One retained observation, compressed to the fields the runtime needs for
/// history, windows, classification and explanation.
///
/// The record deliberately carries no std::string and no heap allocation, so a
/// bounded ring of records has a compile-time known cost per entry.
struct HistoryRecord {
  ReceiveTime received{};
  ObservationTime observed{};
  SourceId source{};
  GenerationId generation{};
  IncarnationOrdinal incarnation_ordinal{0};
  SourceSequence sequence{};
  SourceAuthority authority{SourceAuthority::Unknown};
  FieldMask reported{0};
  Freshness freshness{Freshness::Unknown};
  EvidenceQuality quality{EvidenceQuality::Unknown};
  EvidenceFlags flags{0};
  Nanos age_ns{0};
  std::uint64_t values[kSampleFieldCount]{};
  /// Deltas for counter fields; empty deltas are marked unknown.
  CounterDelta deltas[kSampleFieldCount]{};
  PressureState state{PressureState::Unknown};
  /// Digest of the explanation that produced the state, so that a later
  /// explanation query can prove it is reproducing the same decision.
  std::uint64_t state_digest{0};

  [[nodiscard]] std::optional<std::uint64_t> value(SampleField field) const noexcept {
    const auto index = static_cast<std::size_t>(field);
    if (index >= kSampleFieldCount || !has_field(reported, field)) {
      return std::nullopt;
    }
    return values[index];
  }

  [[nodiscard]] const CounterDelta& delta(SampleField field) const noexcept {
    static const CounterDelta kEmpty{};
    const auto index = static_cast<std::size_t>(field);
    return index < kSampleFieldCount ? deltas[index] : kEmpty;
  }

  [[nodiscard]] bool establishes_current() const noexcept {
    EvidenceAssessment assessment;
    assessment.freshness = freshness;
    assessment.quality = quality;
    assessment.authority = authority;
    assessment.flags = flags;
    assessment.age_ns = age_ns;
    return establishes_current_pressure(assessment);
  }
};

/// Fixed-capacity ring of history records.
class HistoryRing {
 public:
  HistoryRing() = default;

  /// Reserve capacity. Called once per queue while the store still holds its
  /// write lock, and only after the byte budget has been checked.
  void reserve(std::size_t capacity);
  void clear() noexcept;

  void push(const HistoryRecord& record);

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0u; }
  [[nodiscard]] std::uint64_t evicted() const noexcept { return evicted_; }
  [[nodiscard]] std::size_t bytes() const noexcept {
    return capacity_ * sizeof(HistoryRecord);
  }

  /// index 0 is the oldest retained record.
  [[nodiscard]] const HistoryRecord& at(std::size_t index) const noexcept;
  [[nodiscard]] const HistoryRecord& newest() const noexcept { return at(size_ - 1u); }

  /// Copy every retained record in ascending time order.
  [[nodiscard]] std::vector<HistoryRecord> snapshot() const;

  /// Copy the records whose receive time is at or after the supplied instant.
  /// The scan is bounded by the ring capacity.
  [[nodiscard]] std::vector<HistoryRecord> since(Nanos steady_ns) const;

 private:
  std::vector<HistoryRecord> storage_{};
  std::size_t head_{0};
  std::size_t size_{0};
  std::size_t capacity_{0};
  std::uint64_t evicted_{0};
};

}  // namespace qobs
