#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <cstdio>
#include <random>
#include <vector>

#include "analytics/hayashi_yoshida.h"
#include "core/rng.h"

using basis::analytics::HayashiYoshidaConfig;
using basis::analytics::HayashiYoshidaEstimator;

namespace {

constexpr std::int64_t kMs = 1'000'000;  // ns per millisecond

// A joint update stream over a latent random walk, in the shape the CLI
// feeds an estimator: one record per venue update, each carrying both
// venues' last known mid. Venue A republishes every a_period_ms; venue B
// republishes every b_period_ms and quotes the latent walk lead_ms late.
// The two periods are deliberately unequal and non-dividing, because that
// asynchrony is what the estimator exists to handle.
struct Joint {
  std::vector<double> a, b;
  std::vector<std::int64_t> ts;
};

Joint make_stream(int duration_ms, int a_period_ms, int b_period_ms,
                  int lead_ms, unsigned seed = 7) {
  // Latent walk on a 1 ms clock, so both venues sample the same process.
  std::mt19937 engine(seed);
  std::vector<double> latent(static_cast<std::size_t>(duration_ms) + 1);
  double price = 100.0;
  for (auto& p : latent) {
    price += basis::rng::normal(engine, 0.0, 0.05);
    p = price;
  }
  const auto at = [&](int ms) {
    if (ms < 0) ms = 0;
    if (ms >= static_cast<int>(latent.size())) ms = static_cast<int>(latent.size()) - 1;
    return latent[static_cast<std::size_t>(ms)];
  };

  Joint out;
  double last_a = at(0), last_b = at(-lead_ms);
  for (int ms = 0; ms <= duration_ms; ++ms) {
    const bool a_tick = (ms % a_period_ms) == 0;
    const bool b_tick = (ms % b_period_ms) == 0;
    if (!a_tick && !b_tick) continue;
    if (a_tick) last_a = at(ms);
    if (b_tick) last_b = at(ms - lead_ms);
    out.a.push_back(last_a);
    out.b.push_back(last_b);
    out.ts.push_back(static_cast<std::int64_t>(ms) * kMs);
  }
  return out;
}

HayashiYoshidaEstimator feed(const Joint& j, HayashiYoshidaConfig cfg) {
  HayashiYoshidaEstimator est(cfg);
  for (std::size_t i = 0; i < j.ts.size(); ++i) {
    est.observe(j.a[i], j.b[i], j.ts[i]);
  }
  return est;
}

}  // namespace

TEST(HayashiYoshida, RecoversInjectedLead) {
  // A leads B by 400 ms; the venues update on 30 ms and 70 ms clocks, so
  // their observation times almost never coincide.
  const auto stream = make_stream(600'000, 30, 70, 400);
  const auto res = feed(stream, HayashiYoshidaConfig{
                                    .lag_step_ns = 10 * kMs,
                                    .max_lag_steps = 120}).estimate();
  EXPECT_NEAR(res.lead_seconds, 0.4, 0.05);
  EXPECT_GT(res.correlation, 0.2);
  EXPECT_GT(res.resamples, 0u);
  EXPECT_TRUE(res.lead_is_significant());
}

TEST(HayashiYoshida, SignFlipsWithTheLeader) {
  // Same construction with the roles swapped: B quotes the walk first, so
  // the reported lead must be negative and of the same size.
  const auto stream = make_stream(600'000, 30, 70, -400);
  const auto res = feed(stream, HayashiYoshidaConfig{
                                    .lag_step_ns = 10 * kMs,
                                    .max_lag_steps = 120}).estimate();
  EXPECT_NEAR(res.lead_seconds, -0.4, 0.05);
  EXPECT_TRUE(res.lead_is_significant());
}

TEST(HayashiYoshida, ResolvesBelowTheCrossCorrelationGrid) {
  // 120 ms is under a bin of the 200 ms grid the cross-correlation
  // estimator would need here, which is the whole reason this estimator
  // exists: no resampling, so no floor at one bin.
  const auto stream = make_stream(900'000, 30, 70, 120);
  const auto res = feed(stream, HayashiYoshidaConfig{
                                    .lag_step_ns = 5 * kMs,
                                    .max_lag_steps = 160}).estimate();
  EXPECT_NEAR(res.lead_seconds, 0.12, 0.05);
  EXPECT_TRUE(res.lead_is_significant());
}

TEST(HayashiYoshida, NoLeadIsNotResolved) {
  // Both venues quote the same walk with no offset. The point estimate
  // should sit at zero and, more importantly, the interval must straddle
  // it: claiming a direction here would be the failure mode.
  const auto stream = make_stream(600'000, 30, 70, 0);
  const auto res = feed(stream, HayashiYoshidaConfig{
                                    .lag_step_ns = 10 * kMs,
                                    .max_lag_steps = 120}).estimate();
  EXPECT_NEAR(res.lead_seconds, 0.0, 0.05);
  EXPECT_FALSE(res.lead_is_significant());
}

TEST(HayashiYoshida, IndependentSeriesResolveNothing) {
  // Two unrelated walks. There is no lead to find, and the estimator must
  // not manufacture one that survives its own interval.
  const auto x = make_stream(400'000, 30, 70, 0, 11);
  const auto y = make_stream(400'000, 30, 70, 0, 99);
  HayashiYoshidaEstimator est(HayashiYoshidaConfig{.lag_step_ns = 10 * kMs,
                                                   .max_lag_steps = 100});
  const auto n = std::min(x.ts.size(), y.ts.size());
  for (std::size_t i = 0; i < n; ++i) est.observe(x.a[i], y.b[i], x.ts[i]);
  const auto res = est.estimate();
  EXPECT_LT(res.correlation, 0.25);
  EXPECT_FALSE(res.lead_is_significant());
}

TEST(HayashiYoshida, DegenerateInputsAreRefusedNotGuessed) {
  HayashiYoshidaEstimator empty{};
  EXPECT_EQ(empty.estimate().samples, 0u);
  EXPECT_EQ(empty.estimate().lead_seconds, 0.0);

  // A flat venue carries no timing information at all.
  HayashiYoshidaEstimator flat{};
  for (int i = 0; i < 500; ++i) {
    flat.observe(100.0, 100.0 + 0.01 * i, static_cast<std::int64_t>(i) * kMs);
  }
  const auto flat_res = flat.estimate();
  EXPECT_EQ(flat_res.correlation, 0.0);
  EXPECT_EQ(flat_res.resamples, 0u);

  // Timestamps that would overflow the lag arithmetic are refused rather
  // than wrapped into a plausible-looking answer.
  HayashiYoshidaEstimator wild{};
  wild.observe(1.0, 1.0, 0);
  wild.observe(2.0, 2.0, 1);
  wild.observe(3.0, 3.0, std::numeric_limits<std::int64_t>::max() / 2);
  EXPECT_EQ(wild.estimate().lead_seconds, 0.0);
}

TEST(HayashiYoshida, ExactUnderAFollowerThatCoalesces) {
  // The follower republishes on a 50 ms timer while the leader pushes
  // every 5 ms, so the follower's observation intervals are ten times
  // wider than the leader's and the injected lead is smaller than one of
  // them. That is the shape of a real venue pair, and it is where a
  // grid-resampling estimator has nothing left to say: the lead is inside
  // a single bin. Hayashi-Yoshida is exact to the scan step here.
  for (const int lead_ms : {10, 20, 50, 100, 200}) {
    const auto stream = make_stream(900'000, 5, 50, lead_ms);
    const auto rep = feed(stream, HayashiYoshidaConfig{
                                      .lag_step_ns = 5 * kMs,
                                      .max_lag_steps = 200}).analyze();
    EXPECT_NEAR(rep.lead.lead_seconds * 1000.0, lead_ms, 5.0)
        << "injected lead " << lead_ms << " ms";
    EXPECT_GT(rep.ratio, 1.0) << "injected lead " << lead_ms << " ms";
    EXPECT_TRUE(rep.ratio_resolved()) << "injected lead " << lead_ms << " ms";
  }
}

TEST(HayashiYoshida, RatioStraddlesOneWithNoLead) {
  // The mirror of the above: no injected lead, so the ratio must not
  // resolve a direction. This is the assertion that would catch a sign
  // convention or normalization bug turning noise into a finding.
  const auto stream = make_stream(900'000, 5, 50, 0);
  const auto rep = feed(stream, HayashiYoshidaConfig{
                                    .lag_step_ns = 5 * kMs,
                                    .max_lag_steps = 200}).analyze();
  EXPECT_NEAR(rep.ratio, 1.0, 0.15);
  EXPECT_FALSE(rep.ratio_resolved());
}
