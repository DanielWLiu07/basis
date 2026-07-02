#pragma once

#include <cstdint>

namespace basis::analytics {

// Running statistics over one event's basis (Kalshi mid minus Polymarket
// mid, in cents). The accessors other than samples() require at least one
// observation.
class DivergenceTracker {
 public:
  void observe(double basis_cents) {
    ++samples_;
    sum_ += basis_cents;
    last_ = basis_cents;
    if (samples_ == 1 || basis_cents < min_) min_ = basis_cents;
    if (samples_ == 1 || basis_cents > max_) max_ = basis_cents;
  }

  std::uint64_t samples() const { return samples_; }
  double mean() const { return sum_ / static_cast<double>(samples_); }
  double min() const { return min_; }
  double max() const { return max_; }
  double last() const { return last_; }

 private:
  std::uint64_t samples_ = 0;
  double sum_ = 0.0;
  double last_ = 0.0;
  double min_ = 0.0;
  double max_ = 0.0;
};

}  // namespace basis::analytics
