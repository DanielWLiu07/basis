#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <random>

#include "core/rng.h"

using basis::rng::uniform_int;

TEST(Rng, UniformIntStaysInRange) {
  std::mt19937 engine(123);
  for (int i = 0; i < 10000; ++i) {
    const auto v = uniform_int(engine, -5, 5);
    EXPECT_GE(v, -5);
    EXPECT_LE(v, 5);
  }
}

TEST(Rng, UniformIntSinglePointReturnsThatPoint) {
  std::mt19937 engine(1);
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(uniform_int(engine, 7, 7), 7);
  }
}

TEST(Rng, UniformIntHandlesExtremeRangesWithoutOverflow) {
  // hi - lo here exceeds the signed range; computing it in signed int64
  // would be UB, and the full 64-bit interval must not wrap to a modulo by
  // zero. Just needs to return an in-range value without a fault.
  std::mt19937 engine(42);
  constexpr auto lo = std::numeric_limits<std::int64_t>::min();
  constexpr auto hi = std::numeric_limits<std::int64_t>::max();
  for (int i = 0; i < 1000; ++i) {
    const auto v = uniform_int(engine, lo, hi);
    EXPECT_GE(v, lo);
    EXPECT_LE(v, hi);
  }
  // A wide-but-not-full range that still overflows a signed subtraction.
  for (int i = 0; i < 1000; ++i) {
    const auto v = uniform_int(engine, lo, 0);
    EXPECT_GE(v, lo);
    EXPECT_LE(v, 0);
  }
}

TEST(Rng, UniformIntIsDeterministicForASeed) {
  std::mt19937 a(99);
  std::mt19937 b(99);
  for (int i = 0; i < 50; ++i) {
    EXPECT_EQ(uniform_int(a, -100, 100), uniform_int(b, -100, 100));
  }
}
