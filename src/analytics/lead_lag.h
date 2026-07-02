#pragma once

#include <cstdint>

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

}  // namespace basis::analytics
