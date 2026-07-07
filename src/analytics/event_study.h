#pragma once

#include <cstdint>
#include <vector>

namespace basis::analytics {

struct EventStudyConfig {
  double move_cents = 1.0;              // a repricing, not book noise
  std::int64_t follow_window_ns = 10'000'000'000;  // 10 s to answer
};

struct EventStudyResult {
  // Venue A moves, venue B follows in the same direction within the
  // window. median_follow_seconds is over the followed moves only.
  std::uint64_t moves = 0;
  std::uint64_t followed = 0;
  double median_follow_seconds = 0.0;

  // The mirror direction. If A really leads, its moves get followed and
  // B's mostly do not (B's moves are late copies A already made).
  std::uint64_t reverse_moves = 0;
  std::uint64_t reverse_followed = 0;
  double reverse_median_follow_seconds = 0.0;
};

// The cross-check the methodology in PLAN.md calls for: instead of
// correlating whole return series, isolate discrete repricings on one
// venue and measure how long the other takes to move the same way. Two
// unrelated methods agreeing on the same lead is what makes the finding
// defensible; one method alone can always be an artifact of its own
// assumptions (grid size, block length, threshold).
class EventStudyEstimator {
 public:
  explicit EventStudyEstimator(EventStudyConfig config = {})
      : config_(config) {}

  // Timestamps must be non-decreasing (receive order), same contract as
  // the cross-correlation estimator.
  void observe(double a_mid, double b_mid, std::int64_t ts_ns);
  EventStudyResult estimate() const;

 private:
  struct Sample {
    std::int64_t ts_ns;
    double a;
    double b;
  };

  EventStudyConfig config_;
  std::vector<Sample> samples_;
};

}  // namespace basis::analytics
