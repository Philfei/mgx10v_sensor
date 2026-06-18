#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>

namespace slam {

template <typename T>
class BoundedFrameQueue {
public:
  explicit BoundedFrameQueue(size_t capacity) : capacity_(capacity) {
    if (capacity_ == 0) capacity_ = 1;
  }

  bool push(T item) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return false;
    if (queue_.size() >= capacity_) {
      queue_.pop_front();
      ++dropped_count_;
    }
    queue_.push_back(std::move(item));
    cond_.notify_one();
    return true;
  }

  bool pop(T* item) {
    if (!item) return false;
    std::unique_lock<std::mutex> lock(mutex_);
    cond_.wait(lock, [&] { return closed_ || !queue_.empty(); });
    if (queue_.empty()) return false;
    *item = std::move(queue_.front());
    queue_.pop_front();
    return true;
  }

  void close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    cond_.notify_all();
  }

  uint64_t dropped_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_count_;
  }

private:
  size_t capacity_ = 1;
  mutable std::mutex mutex_;
  std::condition_variable cond_;
  std::deque<T> queue_;
  bool closed_ = false;
  uint64_t dropped_count_ = 0;
};

}  // namespace slam
