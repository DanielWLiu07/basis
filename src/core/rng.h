#pragma once

#include <cmath>
#include <cstdint>
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

// Uniform integer in [lo, hi]. The modulo bias is far below anything the
// synthetic data could resolve.
inline std::int64_t uniform_int(std::mt19937& rng, std::int64_t lo,
                                std::int64_t hi) {
  const auto range = static_cast<std::uint64_t>(hi - lo) + 1;
  const std::uint64_t draw =
      (static_cast<std::uint64_t>(rng()) << 32) | rng();
  return lo + static_cast<std::int64_t>(draw % range);
}

}  // namespace basis::rng
