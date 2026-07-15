#include <gtest/gtest.h>

#include <cmath>
#include <random>

#include "analytics/divergence.h"
#include "core/rng.h"

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

TEST(DivergenceTracker, RecoversDeterministicHalfLife) {
  // A pure geometric decay x_t = 0.5 * x_{t-1} lies exactly on y = 0.5x, so
  // the AR(1) slope is 0.5 and the half-life is ln(0.5)/ln(0.5) = 1 update.
  DivergenceTracker t;
  double x = 128.0;
  t.observe(x);
  for (int i = 0; i < 30; ++i) {
    x *= 0.5;
    t.observe(x);
  }
  EXPECT_NEAR(t.ar1_coefficient(), 0.5, 1e-9);
  EXPECT_TRUE(t.is_mean_reverting());
  EXPECT_NEAR(t.reversion_halflife_updates(), 1.0, 1e-9);
}

TEST(DivergenceTracker, RecoversNoisyAr1Coefficient) {
  // x_t = 0.8 x_{t-1} + noise: the estimated coefficient should land near
  // 0.8 and the half-life near ln(0.5)/ln(0.8) ~= 3.106 updates.
  DivergenceTracker t;
  std::mt19937 engine(7);
  double x = 0.0;
  for (int i = 0; i < 40000; ++i) {
    x = 0.8 * x + basis::rng::normal(engine, 0.0, 1.0);
    t.observe(x);
  }
  EXPECT_NEAR(t.ar1_coefficient(), 0.8, 0.02);
  EXPECT_TRUE(t.is_mean_reverting());
  EXPECT_NEAR(t.reversion_halflife_updates(),
              std::log(0.5) / std::log(0.8), 0.2);
}

TEST(DivergenceTracker, AntiPersistentSeriesIsNotMeanReverting) {
  // x_t = -0.6 x_{t-1} flips sign each step (phi < 0): it is not the
  // pull-back-to-mean the half-life measures, so no finite half-life.
  DivergenceTracker t;
  double x = 100.0;
  t.observe(x);
  for (int i = 0; i < 30; ++i) {
    x *= -0.6;
    t.observe(x);
  }
  EXPECT_NEAR(t.ar1_coefficient(), -0.6, 1e-9);
  EXPECT_FALSE(t.is_mean_reverting());
  EXPECT_DOUBLE_EQ(t.reversion_halflife_updates(), 0.0);
}

TEST(DivergenceTracker, Ar1UndefinedWithoutVariationOrData) {
  DivergenceTracker one;
  one.observe(5.0);  // no pair yet
  EXPECT_DOUBLE_EQ(one.ar1_coefficient(), 0.0);
  EXPECT_DOUBLE_EQ(one.reversion_halflife_updates(), 0.0);

  DivergenceTracker flat;
  for (int i = 0; i < 10; ++i) flat.observe(4.0);  // predictor never varies
  EXPECT_DOUBLE_EQ(flat.ar1_coefficient(), 0.0);
  EXPECT_FALSE(flat.is_mean_reverting());
}
