#include "bench/latency_recorder.h"

#include <algorithm>
#include <cmath>

namespace basis::bench {

namespace {

std::int64_t nearest_rank(const std::vector<std::int64_t>& sorted, double q) {
  const auto n = sorted.size();
  const auto rank = static_cast<std::size_t>(
      std::ceil(q * static_cast<double>(n)));
  return sorted[std::min(n - 1, rank == 0 ? 0 : rank - 1)];
}

}  // namespace

LatencyRecorder::Report LatencyRecorder::report() const {
  Report r;
  r.count = samples_.size();
  if (samples_.empty()) return r;

  std::vector<std::int64_t> sorted = samples_;
  std::sort(sorted.begin(), sorted.end());

  r.min_ns = sorted.front();
  r.max_ns = sorted.back();
  r.p50_ns = nearest_rank(sorted, 0.50);
  r.p90_ns = nearest_rank(sorted, 0.90);
  r.p99_ns = nearest_rank(sorted, 0.99);

  double sum = 0.0;
  for (const auto s : sorted) sum += static_cast<double>(s);
  r.mean_ns = sum / static_cast<double>(sorted.size());
  return r;
}

}  // namespace basis::bench
