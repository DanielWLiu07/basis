#pragma once

#include <cstdint>

namespace basis::bench {

// Per-operation latency distribution in nanoseconds. A mean hides exactly
// what an execution path cares about: the tail. These percentiles are
// measured by timing each operation individually, so every sample carries
// one clock-read pair of overhead; `timer_overhead_ns` is that cost,
// calibrated on the same machine in the same run, and is reported rather
// than subtracted so the numbers stay raw.
struct LatencyPercentiles {
  std::uint64_t samples = 0;
  double p50 = 0.0;
  double p99 = 0.0;
  double p999 = 0.0;
  double max = 0.0;
};

// Matching-engine microbenchmark. Both implementations replay one
// pre-generated, deterministic order flow, so the comparison is a pure
// data-structure measurement: same ops, same order, same seed.
struct LobBenchResult {
  std::uint64_t ops = 0;          // submits + cancels replayed
  std::uint64_t fills = 0;        // matches produced (identical for both)
  std::int64_t  filled_size = 0;  // contracts traded (identical for both)

  double ladder_ns_per_op = 0.0;  // flat 99-slot ladder (production book)
  double map_ns_per_op = 0.0;     // std::map + list baseline
  double speedup = 0.0;           // map / ladder

  // True when the two implementations produced identical fill streams; a
  // false here invalidates the timing comparison, so the caller reports it.
  bool agreed = false;

  // Ladder-book latency split by what the operation actually had to do.
  // Resting quotes and cancels are the O(1) paths; a crossing order walks
  // levels and pays for the liquidity it consumes, so its tail is the one
  // that matters for an aggressor.
  double timer_overhead_ns = 0.0;
  LatencyPercentiles rest_latency;    // submit that rests without crossing
  // The same resting path on a book that was NOT pre-sized, so container
  // growth is left in. The gap between the two maxima is what pre-sizing
  // buys, and it is entirely tail: the medians are identical.
  LatencyPercentiles rest_latency_growing;
  LatencyPercentiles cross_latency;   // submit that matches at least once
  LatencyPercentiles cancel_latency;  // cancel of a live order
};

// n_ops order-flow operations against a book seeded to a realistic depth.
LobBenchResult run_lob_bench(std::uint64_t n_ops, std::uint32_t seed);

}  // namespace basis::bench
