// The only TU in the project that includes BDE headers, compiled as C++17
// to match the dialect Homebrew's BDE archives were built with (bsls
// coerces this at link time). See bde_arena.h for the full rationale.

#include "alloc/bde_arena.h"

#include <bdlma_multipoolallocator.h>
#include <bdlma_sequentialallocator.h>

namespace basis::alloc {

namespace {

using BloombergLP::bdlma::MultipoolAllocator;
using BloombergLP::bdlma::SequentialAllocator;

// bslma::Allocator derives from bsl::memory_resource, which is an alias
// for std::pmr::memory_resource when the native pmr library exists. These
// casts are what let the rest of the engine stay BDE-free.
SequentialAllocator* seq(void* p) { return static_cast<SequentialAllocator*>(p); }
MultipoolAllocator* pool(void* p) { return static_cast<MultipoolAllocator*>(p); }

}  // namespace

BdeSequentialArena::BdeSequentialArena() : impl_(new SequentialAllocator) {}

BdeSequentialArena::~BdeSequentialArena() { delete seq(impl_); }

std::pmr::memory_resource* BdeSequentialArena::resource() {
  return seq(impl_);
}

void BdeSequentialArena::release() { seq(impl_)->release(); }

BdeMultipool::BdeMultipool() : impl_(new MultipoolAllocator) {}

BdeMultipool::~BdeMultipool() { delete pool(impl_); }

std::pmr::memory_resource* BdeMultipool::resource() { return pool(impl_); }

}  // namespace basis::alloc
