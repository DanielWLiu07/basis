#pragma once

#include <cstdint>
#include <vector>

namespace basis::analytics {

struct LeadLagResult {
  double        lead_seconds = 0.0;  // positive: venue A leads venue B
  double        correlation  = 0.0;  // peak cross-correlation at that lag
  std::uint64_t samples      = 0;
};

// Estimates which venue's price moves first for a matched event, by
// cross-correlating the two mid-price return series across a range of lags.
// See PLAN.md "Lead-lag methodology".
class LeadLagEstimator {
 public:
  virtual ~LeadLagEstimator() = default;

  virtual void observe(double a_mid, double b_mid, std::int64_t ts_ns) = 0;
  virtual LeadLagResult estimate() const = 0;
};

struct LeadLagConfig {
  std::int64_t grid_ns = 100'000'000;  // resample interval: 100 ms
  int max_lag_bins = 100;              // scan lags of +-10 s at the default grid
};

// Cross-correlation implementation:
//  1. observe() collects (a_mid, b_mid, ts) samples.
//  2. estimate() resamples both series onto a fixed grid with
//     last-observation-carried-forward, diffs them into returns, and
//     computes Pearson correlation of r_a[t] vs r_b[t + k] for each lag k.
//  3. The k maximizing correlation is how far B trails A:
//     lead_seconds = k * grid, positive when A leads.
// The result is observational; it is never a causal claim.
class CrossCorrelationEstimator final : public LeadLagEstimator {
 public:
  explicit CrossCorrelationEstimator(LeadLagConfig config = {})
      : config_(config) {}

  // Timestamps must be non-decreasing; the feed and replay paths both
  // deliver updates in receive order.
  void observe(double a_mid, double b_mid, std::int64_t ts_ns) override;
  LeadLagResult estimate() const override;

 private:
  struct Sample {
    std::int64_t ts_ns;
    double a;
    double b;
  };

  LeadLagConfig config_;
  std::vector<Sample> samples_;
};

}  // namespace basis::analytics
