#pragma once

#include <cstdint>
#include <string>

namespace qobs {

// ---------------------------------------------------------------------------
// Lock ranks
// ---------------------------------------------------------------------------
// Queue Observatory uses a small, fixed set of lock ranks. The rules are:
//
//   1. A thread acquires locks in strictly increasing rank order.
//   2. Rank kRankLeaf may be acquired while holding any rank, and any rank may
//      be acquired while holding only leaf ranks. Leaf locks never acquire
//      another lock while held.
//   3. No non-leaf rank may be acquired while the same rank is already held by
//      the same thread (no same-rank nesting, no recursion).
//   4. No lock may be held across a callback boundary. Callbacks registered by
//      callers are always invoked after every lock has been released.
//
// The audit below makes those rules machine-checkable rather than aspirational.
inline constexpr int kRankNone = 0;
inline constexpr int kRankLeaf = 1;
inline constexpr int kRankSources = 10;
inline constexpr int kRankStore = 20;
inline constexpr int kRankPersistence = 30;
inline constexpr int kRankTransport = 40;
inline constexpr int kRankWorker = 50;

[[nodiscard]] const char* lock_rank_name(int rank) noexcept;

/// Thread-local locker model plus global violation counters.
///
/// The audit never throws and never aborts: violations are recorded so that a
/// test can assert the count is zero, and so that a deliberately provoked
/// violation can be observed. This keeps the detector itself under test.
class LockAudit final {
 public:
  LockAudit() = delete;

  static void enter(int rank, const char* name) noexcept;
  static void leave(int rank) noexcept;

  /// Announce that a caller-supplied callback is about to run. Reaching this
  /// point while any non-leaf rank is held is a violation.
  static void note_callback(const char* name) noexcept;

  [[nodiscard]] static bool holds_any() noexcept;
  [[nodiscard]] static int highest_rank() noexcept;
  [[nodiscard]] static int depth() noexcept;

  [[nodiscard]] static std::uint64_t descending_acquisitions() noexcept;
  [[nodiscard]] static std::uint64_t same_rank_acquisitions() noexcept;
  [[nodiscard]] static std::uint64_t callbacks_under_lock() noexcept;
  [[nodiscard]] static std::uint64_t unbalanced_releases() noexcept;
  [[nodiscard]] static std::uint64_t total_violations() noexcept;

  /// Zero every counter. Used at the start of a test phase.
  static void reset() noexcept;

  /// True when QOBS_ENABLE_LOCK_AUDIT was enabled at compile time.
  [[nodiscard]] static bool enabled() noexcept;

  /// Deterministic one-line summary of the counters.
  [[nodiscard]] static std::string snapshot();

 private:
  static void record_descending(int rank, int highest, const char* name) noexcept;
  static void record_same_rank(int rank, const char* name) noexcept;
};

/// RAII helper that reports acquisition and release to the audit.
class LockRankGuard final {
 public:
  LockRankGuard(int rank, const char* name) noexcept;
  ~LockRankGuard();

  LockRankGuard(const LockRankGuard&) = delete;
  LockRankGuard& operator=(const LockRankGuard&) = delete;
  LockRankGuard(LockRankGuard&&) = delete;
  LockRankGuard& operator=(LockRankGuard&&) = delete;

 private:
  int rank_{kRankNone};
};

}  // namespace qobs
