#pragma once

#include <algorithm>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace multiplayer::platform {

template <typename Value>
class LatestValueMailbox {
 public:
  void publish(Value value) {
    std::lock_guard lock(mutex_);
    value_ = std::move(value);
  }

  std::optional<Value> take() {
    std::lock_guard lock(mutex_);
    auto value = std::move(value_);
    value_.reset();
    return value;
  }

  void clear() {
    std::lock_guard lock(mutex_);
    value_.reset();
  }

 private:
  std::mutex mutex_;
  std::optional<Value> value_;
};

template <typename Value, size_t Capacity>
class BoundedMailbox {
 public:
  static_assert(Capacity != 0);

  bool push_back(const std::span<const Value> values) {
    std::lock_guard lock(mutex_);
    if (values.size() > Capacity - values_.size())
      return false;
    values_.insert(values_.end(), values.begin(), values.end());
    return true;
  }

  bool push_front(const std::span<const Value> values) {
    std::lock_guard lock(mutex_);
    if (values.size() > Capacity - values_.size())
      return false;
    values_.insert(values_.begin(), values.begin(), values.end());
    return true;
  }

  std::vector<Value> take(const size_t maximum) {
    std::lock_guard lock(mutex_);
    const size_t count = std::min(maximum, values_.size());
    std::vector<Value> result;
    result.reserve(count);
    for (size_t index = 0; index < count; ++index) {
      result.push_back(std::move(values_.front()));
      values_.pop_front();
    }
    return result;
  }

  size_t size() const {
    std::lock_guard lock(mutex_);
    return values_.size();
  }

  size_t remaining() const {
    std::lock_guard lock(mutex_);
    return Capacity - values_.size();
  }

  template <typename Predicate>
  size_t erase_if(Predicate predicate) {
    std::lock_guard lock(mutex_);
    return std::erase_if(values_, std::move(predicate));
  }

  void clear() {
    std::lock_guard lock(mutex_);
    values_.clear();
  }

 private:
  mutable std::mutex mutex_;
  std::deque<Value> values_;
};

}  // namespace multiplayer::platform
