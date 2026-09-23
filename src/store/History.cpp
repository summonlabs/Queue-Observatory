#include "qobs/store/History.hpp"

namespace qobs {
namespace {

const HistoryRecord& empty_record() {
  static const HistoryRecord kEmpty{};
  return kEmpty;
}

}  // namespace

void HistoryRing::reserve(std::size_t capacity) {
  storage_.assign(capacity, HistoryRecord{});
  head_ = 0;
  size_ = 0;
  capacity_ = capacity;
  evicted_ = 0;
}

void HistoryRing::clear() noexcept {
  head_ = 0;
  size_ = 0;
  evicted_ = 0;
}

void HistoryRing::push(const HistoryRecord& record) {
  if (capacity_ == 0u || storage_.empty()) {
    ++evicted_;
    return;
  }
  if (size_ < capacity_) {
    storage_[(head_ + size_) % capacity_] = record;
    ++size_;
    return;
  }
  storage_[head_] = record;
  head_ = (head_ + 1u) % capacity_;
  ++evicted_;
}

const HistoryRecord& HistoryRing::at(std::size_t index) const noexcept {
  if (index >= size_ || capacity_ == 0u) {
    return empty_record();
  }
  return storage_[(head_ + index) % capacity_];
}

std::vector<HistoryRecord> HistoryRing::snapshot() const {
  std::vector<HistoryRecord> out;
  out.reserve(size_);
  for (std::size_t index = 0; index < size_; ++index) {
    out.push_back(at(index));
  }
  return out;
}

std::vector<HistoryRecord> HistoryRing::since(Nanos steady_ns) const {
  std::vector<HistoryRecord> out;
  // Records are stored in ascending receive order, so a linear scan from the
  // newest end stops as soon as it steps past the cutoff. The scan is bounded
  // by the ring capacity.
  for (std::size_t offset = 0; offset < size_; ++offset) {
    const HistoryRecord& record = at(size_ - 1u - offset);
    if (record.received.steady.ns < steady_ns) {
      break;
    }
    out.push_back(record);
  }
  // Reverse into ascending order.
  for (std::size_t left = 0, right = out.size(); left + 1u < right; ++left, --right) {
    std::swap(out[left], out[right - 1u]);
  }
  return out;
}

}  // namespace qobs
