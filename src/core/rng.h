#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>

namespace basis::rng {

// std::mt19937's raw output is standardized; the <random> distributions are
// not, and libstdc++/libc++ produce different sequences from the same seed.
// Anything that must be reproducible across platforms (synthetic sessions,
// the tests pinned to them) derives its values from raw draws through these
// helpers instead.

// Uniform double in [0, 1). 32 bits of resolution, plenty here.
inline double uniform01(std::mt19937& rng) {
  return static_cast<double>(rng()) * (1.0 / 4294967296.0);
}

// Normal via Box-Muller. log(1 - u1) is finite because u1 < 1.
inline double normal(std::mt19937& rng, double mean, double stddev) {
  constexpr double kTwoPi = 6.283185307179586;
  const double u1 = uniform01(rng);
  const double u2 = uniform01(rng);
  return mean +
         stddev * std::sqrt(-2.0 * std::log(1.0 - u1)) * std::cos(kTwoPi * u2);
}

// Uniform integer in [lo, hi], lo <= hi. The modulo bias is far below
// anything the synthetic data could resolve.
inline std::int64_t uniform_int(std::mt19937& rng, std::int64_t lo,
                                std::int64_t hi) {
  // Draw into named locals on separate statements: the two rng() calls
  // are unsequenced as operands of |, so combining them inline would let
  // the compiler pick which 32-bit half is drawn first, and this helper
  // exists precisely to keep the sequence identical across toolchains.
  const std::uint64_t high = static_cast<std::uint64_t>(rng());
  const std::uint64_t low = static_cast<std::uint64_t>(rng());
  const std::uint64_t draw = (high << 32) | low;
  // Width of [lo, hi] as an unsigned count minus one, computed in unsigned
  // so hi - lo cannot overflow the signed range. When the interval spans
  // the whole int64 range this is UINT64_MAX, i.e. every 64-bit value is
  // in range, so the draw passes through unreduced.
  const std::uint64_t span =
      static_cast<std::uint64_t>(hi) - static_cast<std::uint64_t>(lo);
  if (span == std::numeric_limits<std::uint64_t>::max()) {
    return static_cast<std::int64_t>(draw);
  }
  return lo + static_cast<std::int64_t>(draw % (span + 1));
}

}  // namespace basis::rng
