#pragma once

#include <cmath>
#include <cstdint>

namespace basis::analytics {

// Running statistics over one event's basis (Kalshi mid minus Polymarket
// mid, in cents). Mean and variance use Welford's online recurrence, which
// stays numerically stable over a long session without storing samples.
// The accessors other than samples() require at least one observation.
class DivergenceTracker {
 public:
  void observe(double basis_cents) {
    ++samples_;
    last_ = basis_cents;
    if (samples_ == 1 || basis_cents < min_) min_ = basis_cents;
    if (samples_ == 1 || basis_cents > max_) max_ = basis_cents;
    const double delta = basis_cents - mean_;
    mean_ += delta / static_cast<double>(samples_);
    m2_ += delta * (basis_cents - mean_);
  }

  std::uint64_t samples() const { return samples_; }
  double mean() const { return mean_; }
  double min() const { return min_; }
  double max() const { return max_; }
  double last() const { return last_; }

  // Sample standard deviation (n-1). Zero until there are two samples,
  // since spread is undefined for one point.
  double stddev() const {
    if (samples_ < 2) return 0.0;
    return std::sqrt(m2_ / static_cast<double>(samples_ - 1));
  }

 private:
  std::uint64_t samples_ = 0;
  double last_ = 0.0;
  double min_ = 0.0;
  double max_ = 0.0;
  double mean_ = 0.0;  // Welford running mean
  double m2_ = 0.0;    // Welford sum of squared deltas
};

}  // namespace basis::analytics
