#pragma once

#include <cstdint>

namespace basis::bench {

// Subscription fan-out under a slow consumer: the market-data distribution
// problem. One publisher, many subscribers, some of them unable to keep up.
// Both sessions carry the identical workload so the difference is the
// delivery model and nothing else.
struct FanoutBenchResult {
  std::uint64_t subscribers = 0;
  std::uint64_t slow_subscribers = 0;
  std::uint64_t updates = 0;
  std::uint64_t slow_handler_us = 0;  // work each slow handler does per call

  // Publisher-side wall time for the same update stream. The synchronous
  // session runs handlers inline, so a slow consumer's cost lands here;
  // the conflating session slots values and returns.
  double sync_publish_ms = 0.0;
  double conflating_publish_ms = 0.0;
  double speedup = 0.0;  // sync / conflating

  double sync_updates_per_sec = 0.0;
  double conflating_updates_per_sec = 0.0;

  // Conflating session only: how many slotted values were superseded
  // before their subscriber read them. High is healthy under load - it is
  // the mechanism, not a loss.
  std::uint64_t conflated = 0;
  std::uint64_t delivered = 0;

  // Staleness of the last value each subscriber saw, in updates behind the
  // final published value. Conflation keeps this at 0 for every subscriber
  // that drains at all, which is the correctness claim: a slow consumer
  // gets the CURRENT price, not an old one.
  std::uint64_t worst_staleness_updates = 0;
};

FanoutBenchResult run_fanout_bench(std::uint64_t subscribers,
                                   std::uint64_t slow_subscribers,
                                   std::uint64_t updates,
                                   std::uint64_t slow_handler_us);

}  // namespace basis::bench
