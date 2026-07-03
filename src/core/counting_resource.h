#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>

namespace basis::core {

// Forwards to an upstream memory_resource and counts what passes through.
// This is how allocator claims get measured instead of asserted: wrap the
// resource a component uses, run the workload, read the counters. Counters
// are plain integers; wrap per-thread if the workload is not single-threaded.
class CountingResource final : public std::pmr::memory_resource {
 public:
  explicit CountingResource(
      std::pmr::memory_resource* upstream = std::pmr::get_default_resource())
      : upstream_(upstream) {}

  std::uint64_t allocations() const { return allocations_; }
  std::uint64_t deallocations() const { return deallocations_; }
  std::uint64_t bytes() const { return bytes_; }

 private:
  void* do_allocate(std::size_t bytes, std::size_t alignment) override {
    ++allocations_;
    bytes_ += bytes;
    return upstream_->allocate(bytes, alignment);
  }

  void do_deallocate(void* p, std::size_t bytes,
                     std::size_t alignment) override {
    ++deallocations_;
    upstream_->deallocate(p, bytes, alignment);
  }

  bool do_is_equal(
      const std::pmr::memory_resource& other) const noexcept override {
    return this == &other;
  }

  std::pmr::memory_resource* upstream_;
  std::uint64_t allocations_ = 0;
  std::uint64_t deallocations_ = 0;
  std::uint64_t bytes_ = 0;
};

}  // namespace basis::core
