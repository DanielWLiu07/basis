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

    // Feed the AR(1) regression a (previous, current) pair each update. The
    // predictor and response co-moments accumulate Welford-style so the slope
    // stays stable over a long session, same as the mean above.
    if (have_prev_) {
      ++pairs_;
      const double dx = prev_ - ar_mx_;
      const double dy = basis_cents - ar_my_;
      ar_mx_ += dx / static_cast<double>(pairs_);
      ar_my_ += dy / static_cast<double>(pairs_);
      ar_cxx_ += dx * (prev_ - ar_mx_);
      ar_cxy_ += dx * (basis_cents - ar_my_);
    }
    prev_ = basis_cents;
    have_prev_ = true;
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

  // How many standard deviations the latest observation sits from the
  // running mean: a quick read on whether the basis is currently at a
  // typical or an unusual level. Zero when the spread is undefined.
  double zscore() const {
    const double sd = stddev();
    return sd > 0.0 ? (last_ - mean_) / sd : 0.0;
  }

  // AR(1) autoregressive coefficient phi: the OLS slope of basis[t] on
  // basis[t-1]. phi near 1 is a slow-moving (near random-walk) basis; a
  // smaller positive phi snaps back to the mean faster; phi <= 0 means the
  // series flips sign each step rather than reverting. Zero when there is too
  // little data or the previous value never varies (no predictor variance).
  double ar1_coefficient() const {
    if (pairs_ < 2 || ar_cxx_ <= 0.0) return 0.0;
    return ar_cxy_ / ar_cxx_;
  }

  // Mean-reversion half-life in updates: how many book updates it takes the
  // basis to close half the gap back to its mean, ln(0.5)/ln(phi). Defined
  // only for a mean-reverting series (0 < phi < 1); returns 0 otherwise (a
  // random walk or an explosive/oscillating series has no finite half-life).
  // This counts updates, not seconds -- book updates are not evenly spaced in
  // time, so it answers "how many ticks to revert halfway," not a duration.
  double reversion_halflife_updates() const {
    const double phi = ar1_coefficient();
    if (phi <= 0.0 || phi >= 1.0) return 0.0;
    return std::log(0.5) / std::log(phi);
  }

  // True when the basis is mean-reverting (0 < phi < 1): a finite half-life
  // exists and the series pulls back toward its mean rather than wandering.
  bool is_mean_reverting() const {
    const double phi = ar1_coefficient();
    return phi > 0.0 && phi < 1.0;
  }

 private:
  std::uint64_t samples_ = 0;
  double last_ = 0.0;
  double min_ = 0.0;
  double max_ = 0.0;
  double mean_ = 0.0;  // Welford running mean
  double m2_ = 0.0;    // Welford sum of squared deltas

  // Online AR(1) regression of basis[t] (response) on basis[t-1] (predictor).
  bool          have_prev_ = false;
  double        prev_   = 0.0;   // previous observation, the predictor
  std::uint64_t pairs_  = 0;     // (prev, curr) pairs seen
  double        ar_mx_  = 0.0;   // running mean of the predictor
  double        ar_my_  = 0.0;   // running mean of the response
  double        ar_cxx_ = 0.0;   // predictor co-moment  sum (x-mx)^2
  double        ar_cxy_ = 0.0;   // cross co-moment       sum (x-mx)(y-my)
};

}  // namespace basis::analytics
