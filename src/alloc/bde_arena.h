#pragma once

#include <memory_resource>

// Wrappers over Bloomberg bdlma allocators, exposed as std::pmr resources.
//
// This header deliberately includes no BDE headers. Homebrew ships BDE
// built as C++17, and bsls enforces one language dialect per program with
// a linker coercion symbol, so BDE types cannot appear in this project's
// C++20 translation units. Instead, bde_arena.cpp is the one TU compiled
// as C++17 (see src/CMakeLists.txt); it hands the allocators out through
// std::pmr::memory_resource, whose ABI in libc++ and libstdc++ does not
// vary by dialect. Everything else in the engine sees only std types.

namespace basis::alloc {

// Monotonic arena over bdlma::SequentialAllocator: allocation is a bump,
// deallocation is a no-op, release() frees every block at once. Fits the
// per-message parse path, where all allocations die together after the
// deltas are consumed.
class BdeSequentialArena {
 public:
  BdeSequentialArena();
  ~BdeSequentialArena();
  BdeSequentialArena(const BdeSequentialArena&) = delete;
  BdeSequentialArena& operator=(const BdeSequentialArena&) = delete;

  std::pmr::memory_resource* resource();
  void release();

 private:
  void* impl_;
};

// Pool set over bdlma::MultipoolAllocator: buckets of geometrically sized
// pools, so fixed-width map nodes recycle within their bucket instead of
// round-tripping through the global heap. Fits the long-lived order books,
// whose steady state is node churn.
class BdeMultipool {
 public:
  BdeMultipool();
  ~BdeMultipool();
  BdeMultipool(const BdeMultipool&) = delete;
  BdeMultipool& operator=(const BdeMultipool&) = delete;

  std::pmr::memory_resource* resource();

 private:
  void* impl_;
};

}  // namespace basis::alloc
