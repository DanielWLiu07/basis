#pragma once

#include <cstdint>
#include <vector>

namespace basis::bench {

// Collects nanosecond duration samples and reports percentiles. Samples are
// stored raw and sorted once at report time: exact percentiles, no bucket
// error, fine at replay volumes. If sample counts ever make storing them
// silly, this is the seam where an HdrHistogram drops in (PLAN.md Phase 4).
class LatencyRecorder {
 public:
  struct Report {
    std::uint64_t count = 0;
    std::int64_t min_ns = 0;
    std::int64_t p50_ns = 0;
    std::int64_t p90_ns = 0;
    std::int64_t p99_ns = 0;
    std::int64_t max_ns = 0;
    double mean_ns = 0.0;
  };

  void record(std::int64_t ns) { samples_.push_back(ns); }
  std::uint64_t count() const { return samples_.size(); }

  Report report() const;  // nearest-rank percentiles

 private:
  std::vector<std::int64_t> samples_;
};

}  // namespace basis::bench
