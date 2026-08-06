#pragma once

#include <cstdint>

namespace basis::bench {

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
};

// n_ops order-flow operations against a book seeded to a realistic depth.
LobBenchResult run_lob_bench(std::uint64_t n_ops, std::uint32_t seed);

}  // namespace basis::bench
