#include "analytics/hayashi_yoshida.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <random>
#include <vector>

#include "core/rng.h"

namespace basis::analytics {

namespace {

// One venue's own observation, recovered from the joint stream. A return
// spans (begin_ns, end_ns] and is the change in that venue's mid across it.
struct Increment {
  std::int64_t begin_ns;
  std::int64_t end_ns;
  double value;
};

// A venue is observed where its own quote moves. Everything between two of
// its updates is a stretch over which it said nothing, which is precisely
// what Hayashi-Yoshida wants to be told about rather than have filled in.
// Repeats of the same mid fold into the surrounding interval; they carry a
// zero return and contribute nothing either way.
std::vector<Increment> increments_of(const std::vector<double>& mids,
                                     const std::vector<std::int64_t>& ts) {
  std::vector<Increment> out;
  if (mids.size() < 2) return out;
  double last_value = mids.front();
  std::int64_t last_ns = ts.front();
  for (std::size_t i = 1; i < mids.size(); ++i) {
    if (mids[i] == last_value) continue;
    out.push_back({last_ns, ts[i], mids[i] - last_value});
    last_value = mids[i];
    last_ns = ts[i];
  }
  return out;
}

double realized_variance(const std::vector<Increment>& x) {
  double v = 0.0;
  for (const auto& inc : x) v += inc.value * inc.value;
  return v;
}

// Everything both entry points derive from the raw stream before any lag
// is considered. ok = false means the input cannot support an estimate at
// all, and the caller returns its zeroed result rather than a guess.
struct Prepared {
  std::vector<Increment> a;
  std::vector<Increment> b;
  double norm = 0.0;
  bool ok = false;
};

Prepared prepare(const std::vector<double>& a_mids,
                 const std::vector<double>& b_mids,
                 const std::vector<std::int64_t>& ts,
                 const HayashiYoshidaConfig& config) {
  Prepared p;
  if (ts.size() < 3) return p;
  if (config.lag_step_ns <= 0 || config.max_lag_steps <= 0) return p;

  // Timestamps come off a feedlog and can be garbage. Check the span in
  // floating point first: the int64 subtraction would be undefined
  // behavior on extreme values, and the lag arithmetic in the sweep would
  // overflow on the same inputs.
  const double approx_span =
      static_cast<double>(ts.back()) - static_cast<double>(ts.front());
  if (approx_span <= 0.0 || approx_span > 4.0e18) return p;
  const double max_lag_ns = static_cast<double>(config.lag_step_ns) *
                            static_cast<double>(config.max_lag_steps);
  if (max_lag_ns > 4.0e17) return p;

  p.a = increments_of(a_mids, ts);
  p.b = increments_of(b_mids, ts);
  if (p.a.size() < 3 || p.b.size() < 3) return p;

  const double rv_a = realized_variance(p.a);
  const double rv_b = realized_variance(p.b);
  constexpr double kMinVariance = 1e-12;
  if (rv_a < kMinVariance || rv_b < kMinVariance) return p;

  p.norm = std::sqrt(rv_a * rv_b);
  p.ok = true;
  return p;
}

// Sum of dx * dy over every pair of intervals that overlap once b's clock
// is shifted back by lag_ns. Two intervals overlap when each starts before
// the other ends. Both sides are sorted and non-overlapping within a
// venue, so one forward sweep with a rewind cursor visits each pair once.
//
// block_of maps an a-interval to a bootstrap block; when out_blocks is
// non-null the contribution is accumulated per block instead of only
// globally.
double hy_covariance(const std::vector<Increment>& a,
                     const std::vector<Increment>& b, std::int64_t lag_ns,
                     const std::vector<int>* block_of = nullptr,
                     std::vector<double>* out_blocks = nullptr) {
  double total = 0.0;
  std::size_t j = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    // Advance past every b-interval that closed before a[i] opened.
    while (j < b.size() && b[j].end_ns - lag_ns <= a[i].begin_ns) ++j;
    double sum = 0.0;
    for (std::size_t k = j; k < b.size(); ++k) {
      if (b[k].begin_ns - lag_ns >= a[i].end_ns) break;
      sum += a[i].value * b[k].value;
    }
    total += sum;
    if (out_blocks != nullptr && block_of != nullptr) {
      (*out_blocks)[static_cast<std::size_t>((*block_of)[i])] += sum;
    }
  }
  return total;
}

}  // namespace

void HayashiYoshidaEstimator::observe(double a_mid, double b_mid,
                                      std::int64_t ts_ns) {
  samples_.push_back({ts_ns, a_mid, b_mid});
}

std::vector<HayashiYoshidaPoint> HayashiYoshidaEstimator::curve() const {
  std::vector<double> a_mids, b_mids;
  std::vector<std::int64_t> ts;
  a_mids.reserve(samples_.size());
  b_mids.reserve(samples_.size());
  ts.reserve(samples_.size());
  for (const auto& s : samples_) {
    a_mids.push_back(s.a);
    b_mids.push_back(s.b);
    ts.push_back(s.ts_ns);
  }
  const Prepared p = prepare(a_mids, b_mids, ts, config_);
  std::vector<HayashiYoshidaPoint> out;
  if (!p.ok) return out;

  out.reserve(static_cast<std::size_t>(2 * config_.max_lag_steps + 1));
  for (int k = -config_.max_lag_steps; k <= config_.max_lag_steps; ++k) {
    const std::int64_t lag_ns =
        static_cast<std::int64_t>(k) * config_.lag_step_ns;
    out.push_back({lag_ns, hy_covariance(p.a, p.b, lag_ns) / p.norm});
  }
  return out;
}

HayashiYoshidaReport HayashiYoshidaEstimator::analyze() const {
  HayashiYoshidaReport report;
  LeadLagResult& result = report.lead;
  result.samples = samples_.size();

  std::vector<double> a_mids, b_mids;
  std::vector<std::int64_t> ts;
  a_mids.reserve(samples_.size());
  b_mids.reserve(samples_.size());
  ts.reserve(samples_.size());
  for (const auto& s : samples_) {
    a_mids.push_back(s.a);
    b_mids.push_back(s.b);
    ts.push_back(s.ts_ns);
  }
  const Prepared p = prepare(a_mids, b_mids, ts, config_);
  if (!p.ok) return report;

  // A non-overlapping block bootstrap: the a-intervals are cut into
  // `blocks` contiguous runs, and because they are time-ordered each run
  // is a stretch of the session with whatever both venues did during it.
  // Resampling whole blocks keeps the within-block cross-venue timing that
  // is the entire measurement; resampling individual increments would
  // shuffle exactly that away.
  const int blocks =
      std::clamp(config_.bootstrap_blocks, 1,
                 std::max(1, static_cast<int>(p.a.size()) / 4));
  const bool want_bootstrap =
      config_.bootstrap_resamples > 0 && blocks >= 4;
  std::vector<int> block_of(p.a.size());
  for (std::size_t i = 0; i < p.a.size(); ++i) {
    const auto b = static_cast<std::size_t>(blocks) * i / p.a.size();
    block_of[i] = static_cast<int>(std::min<std::size_t>(
        b, static_cast<std::size_t>(blocks) - 1));
  }

  const int lags = 2 * config_.max_lag_steps + 1;
  // Per-lag, per-block covariance contributions, reused by every resample:
  // a resample is then a sum of `blocks` doubles per lag rather than a
  // rerun of the sweep over resampled data.
  std::vector<double> block_cov(
      want_bootstrap ? static_cast<std::size_t>(lags) *
                           static_cast<std::size_t>(blocks)
                     : 0);
  std::vector<double> scratch(
      want_bootstrap ? static_cast<std::size_t>(blocks) : 0);

  double best_corr = 0.0;
  std::int64_t best_lag_ns = 0;
  for (int k = -config_.max_lag_steps; k <= config_.max_lag_steps; ++k) {
    const std::int64_t lag_ns =
        static_cast<std::int64_t>(k) * config_.lag_step_ns;
    double cov = 0.0;
    if (want_bootstrap) {
      std::fill(scratch.begin(), scratch.end(), 0.0);
      cov = hy_covariance(p.a, p.b, lag_ns, &block_of, &scratch);
      const auto row = static_cast<std::size_t>(k + config_.max_lag_steps) *
                       static_cast<std::size_t>(blocks);
      std::copy(scratch.begin(), scratch.end(),
                block_cov.begin() + static_cast<std::ptrdiff_t>(row));
    } else {
      cov = hy_covariance(p.a, p.b, lag_ns);
    }
    const double corr = cov / p.norm;
    if (corr > best_corr) {
      best_corr = corr;
      best_lag_ns = lag_ns;
    }
  }

  result.correlation = best_corr;
  result.lead_seconds = static_cast<double>(best_lag_ns) / 1e9;
  if (!want_bootstrap) return report;

  // The full-sample lead-lag ratio, from the same per-block partials.
  const auto lag_sum = [&](int k, const std::vector<int>* pick) {
    const auto row = static_cast<std::size_t>(k + config_.max_lag_steps) *
                     static_cast<std::size_t>(blocks);
    double cov = 0.0;
    if (pick == nullptr) {
      for (int b = 0; b < blocks; ++b) {
        cov += block_cov[row + static_cast<std::size_t>(b)];
      }
    } else {
      for (const int d : *pick) {
        cov += block_cov[row + static_cast<std::size_t>(d)];
      }
    }
    return cov;
  };
  const auto ratio_of = [&](const std::vector<int>* pick) {
    double pos = 0.0;
    double neg = 0.0;
    for (int k = 1; k <= config_.max_lag_steps; ++k) {
      const double up = lag_sum(k, pick);
      const double down = lag_sum(-k, pick);
      pos += up * up;
      neg += down * down;
    }
    return neg > 0.0 ? pos / neg : 0.0;
  };
  report.ratio = ratio_of(nullptr);

  if (best_corr <= 0.0) return report;

  // Each resample draws `blocks` blocks with replacement and re-argmaxes
  // near the full-sample peak. The normalization stays at its full-sample
  // value on purpose: it does not depend on the lag, so rescaling it per
  // resample would divide every lag by the same number and cannot move an
  // argmax.
  const int best_k = static_cast<int>(best_lag_ns / config_.lag_step_ns);
  const int half = config_.bootstrap_lag_halfwidth_steps > 0
                       ? config_.bootstrap_lag_halfwidth_steps
                       : config_.max_lag_steps;
  const int k_lo = std::max(-config_.max_lag_steps, best_k - half);
  const int k_hi = std::min(config_.max_lag_steps, best_k + half);

  std::mt19937 engine(config_.bootstrap_seed);
  std::vector<int> draw(static_cast<std::size_t>(blocks));
  std::vector<double> leads;
  std::vector<double> ratios;
  leads.reserve(static_cast<std::size_t>(config_.bootstrap_resamples));
  ratios.reserve(static_cast<std::size_t>(config_.bootstrap_resamples));
  for (int r = 0; r < config_.bootstrap_resamples; ++r) {
    for (auto& d : draw) d = rng::uniform_int(engine, 0, blocks - 1);
    // A window that excludes zero must not silently fall back to it, so
    // the seed lag is one that is actually inside the window.
    double top = -std::numeric_limits<double>::infinity();
    std::int64_t top_lag =
        static_cast<std::int64_t>(std::clamp(0, k_lo, k_hi)) *
        config_.lag_step_ns;
    for (int k = k_lo; k <= k_hi; ++k) {
      const double cov = lag_sum(k, &draw);
      if (cov > top) {
        top = cov;
        top_lag = static_cast<std::int64_t>(k) * config_.lag_step_ns;
      }
    }
    leads.push_back(static_cast<double>(top_lag) / 1e9);
    ratios.push_back(ratio_of(&draw));
  }

  const auto interval = [](std::vector<double>& v, double* lo, double* hi) {
    std::sort(v.begin(), v.end());
    const auto rank = [&](double q) {
      const auto idx = static_cast<std::size_t>(
          q * static_cast<double>(v.size() - 1) + 0.5);
      return v[std::min(idx, v.size() - 1)];
    };
    *lo = rank(0.025);
    *hi = rank(0.975);
  };
  interval(leads, &result.ci_low_seconds, &result.ci_high_seconds);
  result.resamples = static_cast<std::uint64_t>(leads.size());
  interval(ratios, &report.ratio_ci_low, &report.ratio_ci_high);
  report.ratio_resamples = static_cast<std::uint64_t>(ratios.size());
  return report;
}

}  // namespace basis::analytics
