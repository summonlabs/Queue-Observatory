#pragma once

#include <atomic>
#include <memory>

namespace qobs {

/// Cooperative cancellation token.
///
/// Queue Observatory never detaches work that it cannot stop. Every background
/// worker polls a token at a bounded work boundary, so shutdown terminates
/// deterministically instead of relying on a timeout.
class StopToken {
 public:
  StopToken() = default;

  [[nodiscard]] bool stop_requested() const noexcept {
    return flag_ != nullptr && flag_->load(std::memory_order_acquire);
  }

  /// True when this token is attached to a stop source. A default-constructed
  /// token can never be cancelled and callers must not treat it as permission
  /// to run forever.
  [[nodiscard]] bool can_be_cancelled() const noexcept { return flag_ != nullptr; }

 private:
  friend class StopSource;
  explicit StopToken(std::shared_ptr<const std::atomic<bool>> flag) : flag_(std::move(flag)) {}

  std::shared_ptr<const std::atomic<bool>> flag_{};
};

/// Owner of a cancellation flag.
class StopSource {
 public:
  StopSource() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

  [[nodiscard]] StopToken token() const noexcept { return StopToken(flag_); }
  void request_stop() noexcept { flag_->store(true, std::memory_order_release); }
  [[nodiscard]] bool stop_requested() const noexcept {
    return flag_->load(std::memory_order_acquire);
  }
  /// Clears the flag. Only legal once every consumer has stopped.
  void reset() noexcept { flag_->store(false, std::memory_order_release); }

 private:
  std::shared_ptr<std::atomic<bool>> flag_;
};

}  // namespace qobs
