#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include "analytics/lead_lag.h"

using basis::analytics::CrossCorrelationEstimator;
using basis::analytics::LeadLagConfig;

namespace {

constexpr std::int64_t kMs = 1'000'000;  // ns per millisecond

// A latent mid-price random walk sampled every step_ms. Venue A quotes it
// immediately; venue B quotes it lag_steps later. Fixed seed: the test is
// deterministic.
struct SyntheticPair {
  std::vector<double> a;
  std::vector<double> b;
};

SyntheticPair make_pair(int steps, int lag_steps, unsigned seed = 42) {
  std::mt19937 rng(seed);
  std::normal_distribution<double> move(0.0, 0.8);
  std::vector<double> latent(static_cast<std::size_t>(steps));
  double price = 50.0;
  for (auto& p : latent) {
    price += move(rng);
    p = price;
  }
  SyntheticPair out;
  out.a = latent;
  out.b.resize(latent.size());
  for (std::size_t i = 0; i < latent.size(); ++i) {
    const auto src = (i < static_cast<std::size_t>(lag_steps))
                         ? 0
                         : i - static_cast<std::size_t>(lag_steps);
    out.b[i] = latent[src];
  }
  return out;
}

}  // namespace

TEST(CrossCorrelationEstimator, RecoversInjectedLead) {
  // A leads B by 3 steps of 100 ms = 300 ms.
  const auto pair = make_pair(2000, 3);
  CrossCorrelationEstimator est(LeadLagConfig{.grid_ns = 100 * kMs,
                                              .max_lag_bins = 20});
  for (std::size_t i = 0; i < pair.a.size(); ++i) {
    est.observe(pair.a[i], pair.b[i],
                static_cast<std::int64_t>(i) * 100 * kMs);
  }
  const auto result = est.estimate();
  EXPECT_NEAR(result.lead_seconds, 0.3, 1e-9);
  EXPECT_GT(result.correlation, 0.9);
  EXPECT_EQ(result.samples, 2000u);
}

TEST(CrossCorrelationEstimator, LeadSignFlipsWhenBLeads) {
  const auto pair = make_pair(2000, 3);
  CrossCorrelationEstimator est(LeadLagConfig{.grid_ns = 100 * kMs,
                                              .max_lag_bins = 20});
  // Feed B as venue A and vice versa: the lead must come back negative.
  for (std::size_t i = 0; i < pair.a.size(); ++i) {
    est.observe(pair.b[i], pair.a[i],
                static_cast<std::int64_t>(i) * 100 * kMs);
  }
  EXPECT_NEAR(est.estimate().lead_seconds, -0.3, 1e-9);
}

TEST(CrossCorrelationEstimator, SimultaneousSeriesHasZeroLead) {
  const auto pair = make_pair(2000, 0);
  CrossCorrelationEstimator est(LeadLagConfig{.grid_ns = 100 * kMs,
                                              .max_lag_bins = 20});
  for (std::size_t i = 0; i < pair.a.size(); ++i) {
    est.observe(pair.a[i], pair.b[i],
                static_cast<std::int64_t>(i) * 100 * kMs);
  }
  const auto result = est.estimate();
  EXPECT_NEAR(result.lead_seconds, 0.0, 1e-9);
  EXPECT_GT(result.correlation, 0.99);
}

TEST(CrossCorrelationEstimator, FlatSeriesReportsNoSignal) {
  CrossCorrelationEstimator est;
  for (int i = 0; i < 100; ++i) {
    est.observe(50.0, 45.0, static_cast<std::int64_t>(i) * 100 * kMs);
  }
  const auto result = est.estimate();
  EXPECT_DOUBLE_EQ(result.correlation, 0.0);
  EXPECT_DOUBLE_EQ(result.lead_seconds, 0.0);
}

TEST(CrossCorrelationEstimator, GarbageTimestampsAreSafe) {
  // recv_ns comes from the feedlog and can be arbitrary; the span must not
  // overflow and the estimator must decline rather than allocate a grid.
  CrossCorrelationEstimator est;
  est.observe(50.0, 45.0, std::numeric_limits<std::int64_t>::min() + 10);
  est.observe(51.0, 46.0, std::numeric_limits<std::int64_t>::max() - 10);
  const auto result = est.estimate();
  EXPECT_DOUBLE_EQ(result.correlation, 0.0);
  EXPECT_DOUBLE_EQ(result.lead_seconds, 0.0);
}

TEST(CrossCorrelationEstimator, TooFewSamplesIsSafe) {
  CrossCorrelationEstimator est;
  EXPECT_DOUBLE_EQ(est.estimate().correlation, 0.0);
  est.observe(50.0, 45.0, 0);
  EXPECT_DOUBLE_EQ(est.estimate().correlation, 0.0);
  EXPECT_EQ(est.estimate().samples, 1u);
}

TEST(CrossCorrelationEstimator, IrregularSamplingStillRecoversLead) {
  // Updates do not arrive on a neat grid in reality; jitter the observation
  // times and rely on the estimator's own resampling.
  const auto pair = make_pair(2000, 4);  // 400 ms lead
  CrossCorrelationEstimator est(LeadLagConfig{.grid_ns = 100 * kMs,
                                              .max_lag_bins = 20});
  std::mt19937 rng(7);
  std::uniform_int_distribution<std::int64_t> jitter(-30 * kMs, 30 * kMs);
  for (std::size_t i = 0; i < pair.a.size(); ++i) {
    const auto ts = static_cast<std::int64_t>(i) * 100 * kMs + jitter(rng);
    est.observe(pair.a[i], pair.b[i], ts < 0 ? 0 : ts);
  }
  const auto result = est.estimate();
  EXPECT_NEAR(result.lead_seconds, 0.4, 0.11);  // within one grid bin
  EXPECT_GT(result.correlation, 0.5);
}
