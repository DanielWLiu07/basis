#pragma once

#include <cmath>
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

  // Share of each venue's moves the other answered inside the window.
  double forward_follow_rate() const {
    return moves > 0 ? static_cast<double>(followed) /
                           static_cast<double>(moves)
                     : 0.0;
  }
  double reverse_follow_rate() const {
    return reverse_moves > 0 ? static_cast<double>(reverse_followed) /
                                   static_cast<double>(reverse_moves)
                             : 0.0;
  }

  // Two-proportion z comparing the forward follow rate to the reverse. If A
  // genuinely leads, its moves get answered far more often than B's late
  // copies do, so this is large and positive. Zero when either direction saw
  // no moves (nothing to compare) or the pooled rate is degenerate. This
  // gives the event study its own significance test rather than leaving the
  // reader to eyeball the four raw counts.
  double follow_rate_z() const {
    if (moves == 0 || reverse_moves == 0) return 0.0;
    const double n1 = static_cast<double>(moves);
    const double n2 = static_cast<double>(reverse_moves);
    const double p1 = static_cast<double>(followed) / n1;
    const double p2 = static_cast<double>(reverse_followed) / n2;
    const double pooled =
        (static_cast<double>(followed) +
         static_cast<double>(reverse_followed)) /
        (n1 + n2);
    const double se =
        std::sqrt(pooled * (1.0 - pooled) * (1.0 / n1 + 1.0 / n2));
    return se > 0.0 ? (p1 - p2) / se : 0.0;
  }

  // Which venue the event study confirms leads, from the follow-rate gap:
  // +1 for A (its moves get answered more), -1 for B, 0 when neither side
  // clears the bar. A move is "confirmed" past one-sided ~2.5% (|z| >= 1.96).
  int confirmed_leader() const {
    const double z = follow_rate_z();
    if (z >= kConfirmZ) return 1;
    if (z <= -kConfirmZ) return -1;
    return 0;
  }

  // The event study confirms A leads when its moves are answered beyond
  // chance. This is the point of the cross-check -- an independent method
  // agreeing with the cross-correlation lead, not a restatement of it.
  bool lead_confirmed() const { return confirmed_leader() == 1; }

  static constexpr double kConfirmZ = 1.96;  // one-sided ~2.5%
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
