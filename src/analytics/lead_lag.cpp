#include "analytics/lead_lag.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <random>

#include "core/rng.h"

namespace basis::analytics {

namespace {

// Pearson correlation of x[i] vs y[i + offset] over the overlapping range.
// Returns 0 when either side has (near) zero variance: a flat series carries
// no timing information.
double lagged_correlation(const std::vector<double>& x,
                          const std::vector<double>& y, int offset) {
  const auto n = static_cast<std::ptrdiff_t>(x.size());
  const std::ptrdiff_t begin = std::max<std::ptrdiff_t>(0, -offset);
  const std::ptrdiff_t end = std::min<std::ptrdiff_t>(n, n - offset);
  const auto count = end - begin;
  if (count < 3) return 0.0;

  double sum_x = 0.0;
  double sum_y = 0.0;
  for (std::ptrdiff_t i = begin; i < end; ++i) {
    sum_x += x[static_cast<std::size_t>(i)];
    sum_y += y[static_cast<std::size_t>(i + offset)];
  }
  const double mean_x = sum_x / static_cast<double>(count);
  const double mean_y = sum_y / static_cast<double>(count);

  double cov = 0.0;
  double var_x = 0.0;
  double var_y = 0.0;
  for (std::ptrdiff_t i = begin; i < end; ++i) {
    const double dx = x[static_cast<std::size_t>(i)] - mean_x;
    const double dy = y[static_cast<std::size_t>(i + offset)] - mean_y;
    cov += dx * dy;
    var_x += dx * dx;
    var_y += dy * dy;
  }
  constexpr double kMinVariance = 1e-12;
  if (var_x < kMinVariance || var_y < kMinVariance) return 0.0;
  return cov / std::sqrt(var_x * var_y);
}

// Best lag within [lo, hi] by peak correlation. Returns the lag; the peak
// correlation goes to *peak when non-null.
int best_lag_in_range(const std::vector<double>& x,
                      const std::vector<double>& y, int lo, int hi,
                      double* peak = nullptr) {
  double best_corr = 0.0;
  int best = 0;
  for (int lag = lo; lag <= hi; ++lag) {
    const double corr = lagged_correlation(x, y, lag);
    if (corr > best_corr) {
      best_corr = corr;
      best = lag;
    }
  }
  if (peak != nullptr) *peak = best_corr;
  return best;
}

}  // namespace

void CrossCorrelationEstimator::observe(double a_mid, double b_mid,
                                        std::int64_t ts_ns) {
  samples_.push_back({ts_ns, a_mid, b_mid});
}

LeadLagResult CrossCorrelationEstimator::estimate() const {
  LeadLagResult result;
  result.samples = samples_.size();
  if (samples_.size() < 2) return result;

  // Timestamps come from the feedlog, so they can be garbage. Check the
  // span in floating point first: the int64 subtraction below would be
  // undefined behavior on extreme values.
  const double approx_span =
      static_cast<double>(samples_.back().ts_ns) -
      static_cast<double>(samples_.front().ts_ns);
  if (approx_span <= 0.0 || approx_span > 4.0e18) return result;
  const std::int64_t span_ns = samples_.back().ts_ns - samples_.front().ts_ns;
  const auto bins =
      static_cast<std::size_t>(span_ns / config_.grid_ns) + 1;
  // A runaway span (bad timestamps) would allocate absurd grids; refuse
  // rather than melt. 10M bins is hours of data at the default grid.
  constexpr std::size_t kMaxBins = 10'000'000;
  if (bins < 4 || bins > kMaxBins) return result;

  // Resample onto the grid, carrying the last observation forward.
  std::vector<double> grid_a(bins);
  std::vector<double> grid_b(bins);
  std::size_t sample_idx = 0;
  double cur_a = samples_.front().a;
  double cur_b = samples_.front().b;
  const std::int64_t t0 = samples_.front().ts_ns;
  for (std::size_t bin = 0; bin < bins; ++bin) {
    const std::int64_t bin_ts =
        t0 + static_cast<std::int64_t>(bin) * config_.grid_ns;
    while (sample_idx < samples_.size() &&
           samples_[sample_idx].ts_ns <= bin_ts) {
      cur_a = samples_[sample_idx].a;
      cur_b = samples_[sample_idx].b;
      ++sample_idx;
    }
    grid_a[bin] = cur_a;
    grid_b[bin] = cur_b;
  }

  // Correlate returns, not levels: two random walks correlate spuriously in
  // levels, while returns isolate who moved and when.
  std::vector<double> ret_a(bins - 1);
  std::vector<double> ret_b(bins - 1);
  for (std::size_t i = 0; i + 1 < bins; ++i) {
    ret_a[i] = grid_a[i + 1] - grid_a[i];
    ret_b[i] = grid_b[i + 1] - grid_b[i];
  }

  const int max_lag = std::min(config_.max_lag_bins,
                               static_cast<int>(ret_a.size()) - 3);
  double best_corr = 0.0;
  const int best_lag =
      best_lag_in_range(ret_a, ret_b, -max_lag, max_lag, &best_corr);

  const double grid_seconds = static_cast<double>(config_.grid_ns) / 1e9;
  result.correlation = best_corr;
  result.lead_seconds = static_cast<double>(best_lag) * grid_seconds;
  if (best_corr <= 0.0) return result;  // no signal, no interval

  // Paired moving-block bootstrap for the interval. Whole blocks of the
  // (a, b) return pairs are resampled together, so each series keeps its
  // own autocorrelation and the pair keeps the cross-venue timing being
  // estimated; the per-resample scan stays near the full-sample peak.
  const auto n = static_cast<int>(ret_a.size());
  const int block = config_.bootstrap_block_bins;
  if (config_.bootstrap_resamples <= 0 || block <= 0 || n < 2 * block) {
    return result;
  }
  const int num_blocks = (n + block - 1) / block;
  const int lag_lo = std::max(-max_lag, best_lag -
                              config_.bootstrap_lag_halfwidth);
  const int lag_hi = std::min(max_lag, best_lag +
                              config_.bootstrap_lag_halfwidth);

  std::mt19937 engine(config_.bootstrap_seed);
  std::vector<double> boot_a(static_cast<std::size_t>(num_blocks) * block);
  std::vector<double> boot_b(boot_a.size());
  std::vector<double> leads;
  leads.reserve(static_cast<std::size_t>(config_.bootstrap_resamples));
  for (int r = 0; r < config_.bootstrap_resamples; ++r) {
    for (int blk = 0; blk < num_blocks; ++blk) {
      const auto start = static_cast<std::size_t>(
          rng::uniform_int(engine, 0, n - block));
      std::copy_n(ret_a.begin() + static_cast<std::ptrdiff_t>(start), block,
                  boot_a.begin() + static_cast<std::ptrdiff_t>(blk) * block);
      std::copy_n(ret_b.begin() + static_cast<std::ptrdiff_t>(start), block,
                  boot_b.begin() + static_cast<std::ptrdiff_t>(blk) * block);
    }
    const int lag_r = best_lag_in_range(boot_a, boot_b, lag_lo, lag_hi);
    leads.push_back(static_cast<double>(lag_r) * grid_seconds);
  }

  std::sort(leads.begin(), leads.end());
  const auto rank = [&](double q) {
    const auto idx = static_cast<std::size_t>(
        q * static_cast<double>(leads.size() - 1) + 0.5);
    return leads[std::min(idx, leads.size() - 1)];
  };
  result.ci_low_seconds = rank(0.025);
  result.ci_high_seconds = rank(0.975);
  result.resamples = static_cast<std::uint64_t>(leads.size());
  return result;
}

}  // namespace basis::analytics
