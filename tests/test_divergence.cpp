#include <gtest/gtest.h>

#include <cmath>

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
  // Sample variance of {3, -1, 2}: deviations 5/3, -7/3, 2/3, squared and
  // summed to 78/9, over n-1 = 2, so stddev = sqrt(13/3).
  EXPECT_NEAR(t.stddev(), std::sqrt(13.0 / 3.0), 1e-12);
}

TEST(DivergenceTracker, StddevUndefinedBelowTwoSamples) {
  DivergenceTracker t;
  EXPECT_DOUBLE_EQ(t.stddev(), 0.0);
  t.observe(5.0);
  EXPECT_DOUBLE_EQ(t.stddev(), 0.0);
}

TEST(DivergenceTracker, StddevIsZeroForAConstantSeries) {
  DivergenceTracker t;
  for (int i = 0; i < 1000; ++i) t.observe(2.5);
  EXPECT_DOUBLE_EQ(t.mean(), 2.5);
  EXPECT_NEAR(t.stddev(), 0.0, 1e-12);
  // No spread, so no defined z-score.
  EXPECT_DOUBLE_EQ(t.zscore(), 0.0);
}

TEST(DivergenceTracker, ZscoreMeasuresTheLastInStddevs) {
  // {3, -1, 2}: mean 4/3, sd sqrt(13/3); last = 2, so z = (2 - 4/3)/sd.
  DivergenceTracker t;
  t.observe(3.0);
  t.observe(-1.0);
  t.observe(2.0);
  const double expected = (2.0 - 4.0 / 3.0) / std::sqrt(13.0 / 3.0);
  EXPECT_NEAR(t.zscore(), expected, 1e-12);
  // Undefined below two samples.
  DivergenceTracker one;
  one.observe(5.0);
  EXPECT_DOUBLE_EQ(one.zscore(), 0.0);
}

TEST(DivergenceTracker, NegativeOnlyBasisKeepsSignedExtremes) {
  DivergenceTracker t;
  t.observe(-2.5);
  t.observe(-4.0);
  EXPECT_DOUBLE_EQ(t.min(), -4.0);
  EXPECT_DOUBLE_EQ(t.max(), -2.5);
}
