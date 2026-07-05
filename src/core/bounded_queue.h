#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace basis::core {

// A bounded multi-producer queue for crossing the IO -> analytics thread
// boundary. Plain mutex and condition variables: the live message rate is
// tens of messages per second per venue, so contention is not a concern
// and clarity wins (the same call the engine made for the worker pool
// question in docs/bench/allocator.md applies here).
//
// Backpressure policy is block-and-count, never drop: a full queue makes
// push() wait, which in the live path simply lets TCP absorb the burst.
// blocked_pushes() says how often that happened, so "the queue never
// filled" is a measured claim. close() releases both sides; a closed
// queue rejects new items but drains what it holds.
template <typename T>
class BoundedQueue {
 public:
  explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {}

  // Blocks while the queue is full. False once the queue is closed (the
  // item is not enqueued).
  bool push(T value) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (items_.size() >= capacity_ && !closed_) {
      ++blocked_pushes_;
      not_full_.wait(lock, [&] {
        return items_.size() < capacity_ || closed_;
      });
    }
    if (closed_) return false;
    items_.push_back(std::move(value));
    if (items_.size() > high_water_) high_water_ = items_.size();
    ++pushed_;
    lock.unlock();
    not_empty_.notify_one();
    return true;
  }

  // Blocks while the queue is empty. Empty optional only after close()
  // with everything drained, so no item is ever lost to shutdown.
  std::optional<T> pop() {
    std::unique_lock<std::mutex> lock(mutex_);
    not_empty_.wait(lock, [&] { return !items_.empty() || closed_; });
    if (items_.empty()) return std::nullopt;
    T value = std::move(items_.front());
    items_.pop_front();
    ++popped_;
    lock.unlock();
    not_full_.notify_one();
    return value;
  }

  void close() {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
    }
    not_empty_.notify_all();
    not_full_.notify_all();
  }

  std::uint64_t pushed() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return pushed_;
  }
  std::uint64_t popped() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return popped_;
  }
  std::uint64_t blocked_pushes() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return blocked_pushes_;
  }
  std::size_t high_water() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return high_water_;
  }

 private:
  const std::size_t capacity_;
  mutable std::mutex mutex_;
  std::condition_variable not_empty_;
  std::condition_variable not_full_;
  std::deque<T> items_;
  bool closed_ = false;
  std::uint64_t pushed_ = 0;
  std::uint64_t popped_ = 0;
  std::uint64_t blocked_pushes_ = 0;
  std::size_t high_water_ = 0;
};

}  // namespace basis::core
