#pragma once

#include <cstdint>
#include <vector>

#include "analytics/lead_lag.h"

namespace basis::analytics {

struct HayashiYoshidaConfig {
  // The lag scan. Unlike the cross-correlation estimator's grid, this step
  // does not resample anything: it only decides how finely the scan is
  // reported, so it can be far smaller than the venues' update interval
  // without inventing observations that were never made.
  std::int64_t lag_step_ns = 5'000'000;  // 5 ms
  int max_lag_steps = 200;               // scan +-1 s at the default step

  // Block bootstrap over contiguous time blocks. Each block's contribution
  // to the estimator is a plain sum, so a resample is a sum of resampled
  // block contributions rather than a rerun over resampled data; the
  // per-block partials are computed once for every lag and reused.
  // 0 resamples disables the interval. The seed makes it reproducible.
  //
  // Blocks must stay long compared to the dependence being measured -- a
  // lead is at most a second or so, and 200 blocks over a 45 minute
  // session is 13 s each -- but past that, more and shorter blocks lower
  // the variance of the resampled sum, so the count is set by that
  // constraint rather than by taste.
  int bootstrap_resamples = 400;
  int bootstrap_blocks = 200;
  // Each resample re-argmaxes within this many steps of the full-sample
  // peak, not over the whole scan. This is an interval for the located
  // lead; letting a resample wander to the far end of a two-second scan
  // would widen it with lags the full sample already rejected. 0 means no
  // restriction. Mirrors bootstrap_lag_halfwidth in LeadLagConfig.
  int bootstrap_lag_halfwidth_steps = 100;
  std::uint32_t bootstrap_seed = 42;
};

// Hayashi-Yoshida lead-lag estimator.
//
// The cross-correlation estimator has a floor it cannot see past: it
// resamples both venues onto a fixed grid, so it can never resolve a lead
// shorter than one bin, and shrinking the bin manufactures empty bins whose
// zero returns bias the correlation toward zero. That is the standard
// synchronization bias (Epps effect) and it is why this repo reports an
// ordering rather than a duration.
//
// Hayashi-Yoshida removes the grid entirely. It sums the products of the
// two series' returns over every pair of observation intervals that
// overlap in time, which is an unbiased covariance estimator for
// asynchronously observed prices -- no interpolation, no last-value carry
// forward, no empty bins. Scanning it over a shifted clock turns it into a
// lead-lag statistic (Hoffmann/Rosenbaum/Yoshida 2013; Huth/Abergel 2014):
// shift venue B's observation times back by theta, recompute, and the theta
// that maximizes the normalized covariance is how far A leads B.
//
// FEED IT RAW UPDATES. The estimator recovers each venue's own observation
// clock from the points where that venue's mid changes, so pre-sampling the
// pair onto a common grid before observe() destroys exactly the asynchrony
// it exists to exploit and reduces it to a slower cross-correlation.
// One point of the estimator's lag scan: the normalized Hayashi-Yoshida
// covariance with venue B's clock shifted back by lag_ns.
struct HayashiYoshidaPoint {
  std::int64_t lag_ns;
  double correlation;
};

struct HayashiYoshidaReport {
  LeadLagResult lead;

  // The lead-lag ratio (Huth and Abergel 2014): the scan's mass on the
  // positive side over its mass on the negative side,
  // sum_{theta>0} HY(theta)^2 / sum_{theta<0} HY(theta)^2. Above 1 means
  // A leads B.
  //
  // This exists because the argmax is the fragile part of the estimator.
  // A real cross-venue lead does not produce a spike at one lag; it tilts
  // the whole curve to one side, and on a broad, noisy peak the argmax
  // picks between near-ties while the tilt stays put. The ratio reads the
  // tilt, so it answers "which venue leads" even where the argmax cannot
  // answer "by exactly how much". The normalization cancels in the ratio.
  double ratio = 0.0;
  double ratio_ci_low = 0.0;
  double ratio_ci_high = 0.0;
  std::uint64_t ratio_resamples = 0;

  // Resolved when the whole interval sits on one side of 1: the direction
  // is confident. An interval containing 1 means the scan is not tilted
  // far enough to tell, whatever the point estimate reads.
  bool ratio_resolved() const {
    return ratio_resamples > 0 &&
           (ratio_ci_low > 1.0 || ratio_ci_high < 1.0);
  }
};

class HayashiYoshidaEstimator final : public LeadLagEstimator {
 public:
  explicit HayashiYoshidaEstimator(HayashiYoshidaConfig config = {})
      : config_(config) {}

  // Timestamps must be non-decreasing.
  void observe(double a_mid, double b_mid, std::int64_t ts_ns) override;
  LeadLagResult estimate() const override { return analyze().lead; }

  // The peak, the ratio, and both bootstrap intervals in one pass. Prefer
  // this over estimate() when both are wanted: the scan is the expensive
  // part and running it twice buys nothing.
  HayashiYoshidaReport analyze() const;

  // The whole scan, not just its peak. A point estimate is only worth as
  // much as the shape it sits on: a sharp peak is a lead, a plateau is an
  // argmax picking between near-ties. Empty when the input is degenerate,
  // for the same reasons estimate() returns a zeroed result.
  std::vector<HayashiYoshidaPoint> curve() const;

 private:
  struct Sample {
    std::int64_t ts_ns;
    double a;
    double b;
  };

  HayashiYoshidaConfig config_;
  std::vector<Sample> samples_;
};

}  // namespace basis::analytics
