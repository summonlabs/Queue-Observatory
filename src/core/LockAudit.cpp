#include "qobs/core/LockAudit.hpp"

#include <array>
#include <atomic>
#include <string>

namespace qobs {
namespace {

constexpr int kMaxTrackedDepth = 64;

struct ThreadState {
  std::array<int, kMaxTrackedDepth> stack{};
  int depth{0};
};

ThreadState& thread_state() noexcept {
  static thread_local ThreadState state;
  return state;
}

std::atomic<std::uint64_t> g_descending{0};
std::atomic<std::uint64_t> g_same_rank{0};
std::atomic<std::uint64_t> g_callback_under_lock{0};
std::atomic<std::uint64_t> g_unbalanced{0};

bool is_leaf(int rank) noexcept { return rank == kRankLeaf; }

}  // namespace

const char* lock_rank_name(int rank) noexcept {
  switch (rank) {
    case kRankNone:
      return "none";
    case kRankLeaf:
      return "leaf";
    case kRankSources:
      return "sources";
    case kRankStore:
      return "store";
    case kRankPersistence:
      return "persistence";
    case kRankTransport:
      return "transport";
    case kRankWorker:
      return "worker";
    default:
      return "unranked";
  }
}

void LockAudit::enter(int rank, const char* name) noexcept {
#if QOBS_ENABLE_LOCK_AUDIT
  ThreadState& state = thread_state();
  if (rank == kRankNone) {
    return;
  }
  if (!is_leaf(rank)) {
    int highest_non_leaf = kRankNone;
    bool same_rank_held = false;
    for (int index = 0; index < state.depth; ++index) {
      const int held = state.stack[static_cast<std::size_t>(index)];
      if (is_leaf(held)) {
        continue;
      }
      if (held == rank) {
        same_rank_held = true;
      }
      if (held > highest_non_leaf) {
        highest_non_leaf = held;
      }
    }
    if (same_rank_held) {
      record_same_rank(rank, name);
    }
    if (highest_non_leaf != kRankNone && rank < highest_non_leaf) {
      record_descending(rank, highest_non_leaf, name);
    }
  }
  if (state.depth < kMaxTrackedDepth) {
    state.stack[static_cast<std::size_t>(state.depth)] = rank;
  }
  ++state.depth;
#else
  (void)rank;
  (void)name;
#endif
}

void LockAudit::leave(int rank) noexcept {
#if QOBS_ENABLE_LOCK_AUDIT
  ThreadState& state = thread_state();
  if (rank == kRankNone) {
    return;
  }
  if (state.depth <= 0) {
    g_unbalanced.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const int top_index = state.depth - 1;
  const int top = top_index < kMaxTrackedDepth ? state.stack[static_cast<std::size_t>(top_index)]
                                               : kRankNone;
  if (top != rank) {
    g_unbalanced.fetch_add(1, std::memory_order_relaxed);
  }
  --state.depth;
#else
  (void)rank;
#endif
}

void LockAudit::note_callback(const char* name) noexcept {
#if QOBS_ENABLE_LOCK_AUDIT
  (void)name;
  const ThreadState& state = thread_state();
  for (int index = 0; index < state.depth && index < kMaxTrackedDepth; ++index) {
    if (!is_leaf(state.stack[static_cast<std::size_t>(index)])) {
      g_callback_under_lock.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
#else
  (void)name;
#endif
}

bool LockAudit::holds_any() noexcept {
#if QOBS_ENABLE_LOCK_AUDIT
  const ThreadState& state = thread_state();
  for (int index = 0; index < state.depth && index < kMaxTrackedDepth; ++index) {
    if (!is_leaf(state.stack[static_cast<std::size_t>(index)])) {
      return true;
    }
  }
  return false;
#else
  return false;
#endif
}

int LockAudit::highest_rank() noexcept {
#if QOBS_ENABLE_LOCK_AUDIT
  const ThreadState& state = thread_state();
  int highest = kRankNone;
  for (int index = 0; index < state.depth && index < kMaxTrackedDepth; ++index) {
    const int held = state.stack[static_cast<std::size_t>(index)];
    if (held > highest) {
      highest = held;
    }
  }
  return highest;
#else
  return kRankNone;
#endif
}

int LockAudit::depth() noexcept {
#if QOBS_ENABLE_LOCK_AUDIT
  return thread_state().depth;
#else
  return 0;
#endif
}

std::uint64_t LockAudit::descending_acquisitions() noexcept {
  return g_descending.load(std::memory_order_relaxed);
}

std::uint64_t LockAudit::same_rank_acquisitions() noexcept {
  return g_same_rank.load(std::memory_order_relaxed);
}

std::uint64_t LockAudit::callbacks_under_lock() noexcept {
  return g_callback_under_lock.load(std::memory_order_relaxed);
}

std::uint64_t LockAudit::unbalanced_releases() noexcept {
  return g_unbalanced.load(std::memory_order_relaxed);
}

std::uint64_t LockAudit::total_violations() noexcept {
  return descending_acquisitions() + same_rank_acquisitions() + callbacks_under_lock() +
         unbalanced_releases();
}

void LockAudit::reset() noexcept {
  g_descending.store(0, std::memory_order_relaxed);
  g_same_rank.store(0, std::memory_order_relaxed);
  g_callback_under_lock.store(0, std::memory_order_relaxed);
  g_unbalanced.store(0, std::memory_order_relaxed);
}

bool LockAudit::enabled() noexcept {
#if QOBS_ENABLE_LOCK_AUDIT
  return true;
#else
  return false;
#endif
}

std::string LockAudit::snapshot() {
  std::string out;
  out.append("lock_audit{enabled=");
  out.append(enabled() ? "true" : "false");
  out.append(", descending=");
  out.append(std::to_string(descending_acquisitions()));
  out.append(", same_rank=");
  out.append(std::to_string(same_rank_acquisitions()));
  out.append(", callback_under_lock=");
  out.append(std::to_string(callbacks_under_lock()));
  out.append(", unbalanced=");
  out.append(std::to_string(unbalanced_releases()));
  out.append("}");
  return out;
}

void LockAudit::record_descending(int rank, int highest, const char* name) noexcept {
  (void)rank;
  (void)highest;
  (void)name;
  g_descending.fetch_add(1, std::memory_order_relaxed);
}

void LockAudit::record_same_rank(int rank, const char* name) noexcept {
  (void)rank;
  (void)name;
  g_same_rank.fetch_add(1, std::memory_order_relaxed);
}

LockRankGuard::LockRankGuard(int rank, const char* name) noexcept : rank_(rank) {
  LockAudit::enter(rank, name);
}

LockRankGuard::~LockRankGuard() { LockAudit::leave(rank_); }

}  // namespace qobs
