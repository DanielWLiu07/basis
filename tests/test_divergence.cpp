#include <gtest/gtest.h>

#include "analytics/divergence.h"

using basis::analytics::DivergenceTracker;

TEST(DivergenceTracker, TracksRunningStats) {
  DivergenceTracker t;
  EXPECT_EQ(t.samples(), 0u);
  t.observe(3.0);
  t.observe(-1.0);
  t.observe(2.0);
  EXPECT_EQ(t.samples(), 3u);
  EXPECT_DOUBLE_EQ(t.mean(), 4.0 / 3.0);
  EXPECT_DOUBLE_EQ(t.min(), -1.0);
  EXPECT_DOUBLE_EQ(t.max(), 3.0);
  EXPECT_DOUBLE_EQ(t.last(), 2.0);
}

TEST(DivergenceTracker, NegativeOnlyBasisKeepsSignedExtremes) {
  DivergenceTracker t;
  t.observe(-2.5);
  t.observe(-4.0);
  EXPECT_DOUBLE_EQ(t.min(), -4.0);
  EXPECT_DOUBLE_EQ(t.max(), -2.5);
}
