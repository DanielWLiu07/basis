#pragma once

#include <chrono>
#include <cstdint>

namespace basis::time {

// Wall clock, ns since the unix epoch. For data timestamps (feedlog
// recv_ns) that must mean the same thing across runs and machines.
inline std::int64_t wall_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Monotonic clock, ns from an arbitrary origin. For measuring durations
// (ingest-to-signal latency); never compare against wall time.
inline std::int64_t mono_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace basis::time
