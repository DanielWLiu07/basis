#include "analytics/event_study.h"

#include <algorithm>
#include <cmath>

namespace basis::analytics {

namespace {

struct Move {
  std::int64_t ts_ns;
  double direction;        // +1 or -1
  double other_at_move;    // the follower's mid when the move happened
  bool followed = false;
  std::int64_t follow_ns = 0;
};

// Detects moves on the leader series and matches follows on the other.
// A move is a change of at least move_cents from the running reference,
// which then resets, so one repricing counts once however many book
// updates it took. A follow is the other venue moving at least
// move_cents in the same direction from where it stood at move time,
// inside the window.
void scan(const std::vector<double>& leader, const std::vector<double>& other,
          const std::vector<std::int64_t>& ts, double move_cents,
          std::int64_t window_ns, std::uint64_t* moves_out,
          std::uint64_t* followed_out, double* median_out) {
  std::vector<Move> moves;
  double reference = leader.empty() ? 0.0 : leader.front();
  std::size_t pending_begin = 0;  // moves before this index are resolved

  for (std::size_t i = 0; i < leader.size(); ++i) {
    // Resolve pending moves against the follower's current mid.
    for (std::size_t m = pending_begin; m < moves.size(); ++m) {
      Move& move = moves[m];
      if (move.followed) continue;
      if (ts[i] - move.ts_ns > window_ns) {
        // Expired unanswered; compact the pending range when possible.
        if (m == pending_begin) ++pending_begin;
        continue;
      }
      const double delta = (other[i] - move.other_at_move) * move.direction;
      if (delta >= move_cents) {
        move.followed = true;
        move.follow_ns = ts[i] - move.ts_ns;
        if (m == pending_begin) ++pending_begin;
      }
    }

    const double change = leader[i] - reference;
    if (std::abs(change) >= move_cents) {
      moves.push_back({.ts_ns = ts[i],
                       .direction = change > 0 ? 1.0 : -1.0,
                       .other_at_move = other[i]});
      reference = leader[i];
    }
  }

  std::vector<double> follows;
  for (const Move& move : moves) {
    if (move.followed) {
      follows.push_back(static_cast<double>(move.follow_ns) / 1e9);
    }
  }
  *moves_out = moves.size();
  *followed_out = follows.size();
  if (!follows.empty()) {
    std::sort(follows.begin(), follows.end());
    *median_out = follows[follows.size() / 2];
  }
}

}  // namespace

void EventStudyEstimator::observe(double a_mid, double b_mid,
                                  std::int64_t ts_ns) {
  samples_.push_back({ts_ns, a_mid, b_mid});
}

EventStudyResult EventStudyEstimator::estimate() const {
  EventStudyResult result;
  if (samples_.size() < 2) return result;

  std::vector<double> a(samples_.size());
  std::vector<double> b(samples_.size());
  std::vector<std::int64_t> ts(samples_.size());
  for (std::size_t i = 0; i < samples_.size(); ++i) {
    a[i] = samples_[i].a;
    b[i] = samples_[i].b;
    ts[i] = samples_[i].ts_ns;
  }

  scan(a, b, ts, config_.move_cents, config_.follow_window_ns, &result.moves,
       &result.followed, &result.median_follow_seconds);
  scan(b, a, ts, config_.move_cents, config_.follow_window_ns,
       &result.reverse_moves, &result.reverse_followed,
       &result.reverse_median_follow_seconds);
  return result;
}

}  // namespace basis::analytics
