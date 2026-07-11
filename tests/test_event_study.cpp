#include <gtest/gtest.h>

#include <cstdint>

#include "analytics/event_study.h"

using basis::analytics::EventStudyConfig;
using basis::analytics::EventStudyEstimator;

namespace {

constexpr std::int64_t kMs = 1'000'000;  // ns per millisecond

// Venue A repricing in 2-cent steps every two seconds; venue B copying
// each move exactly follow_ms later. Sampled every 100 ms so follows land
// between A's moves.
EventStudyEstimator make_stepped(int moves, std::int64_t follow_ms) {
  EventStudyEstimator est;
  double a = 50.0;
  double b = 50.0;
  const std::int64_t horizon_ms = static_cast<std::int64_t>(moves + 1) * 2000;
  std::int64_t next_move_ms = 2000;
  std::int64_t pending_b_ms = -1;
  double pending_b_value = 0.0;
  int made = 0;
  for (std::int64_t t = 0; t <= horizon_ms; t += 100) {
    if (pending_b_ms >= 0 && t >= pending_b_ms) {
      b = pending_b_value;
      pending_b_ms = -1;
    }
    if (made < moves && t >= next_move_ms) {
      a += (made % 2 == 0) ? 2.0 : -2.0;  // alternate directions
      pending_b_ms = t + follow_ms;
      pending_b_value = a;
      next_move_ms += 2000;
      ++made;
    }
    est.observe(a, b, t * kMs);
  }
  return est;
}

}  // namespace

TEST(EventStudyEstimator, MeasuresTheFollowDelay) {
  const auto est = make_stepped(10, 400);
  const auto result = est.estimate();
  EXPECT_EQ(result.moves, 10u);
  EXPECT_EQ(result.followed, 10u);
  // Sampling is 100 ms, so the measured follow can round up one bin.
  EXPECT_NEAR(result.median_follow_seconds, 0.4, 0.11);
}

TEST(EventStudyEstimator, ReverseDirectionShowsTheFollowerIsLate) {
  const auto est = make_stepped(10, 400);
  const auto result = est.estimate();
  // B's moves are late copies; A "follows" them only in the sense that A
  // already moved first. The scan cannot match A moving after B, because
  // A's repricing predates each B move, so the reverse direction shows
  // few or no matched follows.
  EXPECT_EQ(result.reverse_moves, 10u);
  EXPECT_EQ(result.reverse_followed, 0u);
}

TEST(EventStudyEstimator, BookNoiseBelowThresholdIsNotAMove) {
  EventStudyEstimator est(EventStudyConfig{.move_cents = 1.0});
  double a = 50.0;
  for (int i = 0; i < 200; ++i) {
    a += (i % 2 == 0) ? 0.4 : -0.4;  // oscillating spread noise
    est.observe(a, 45.0, static_cast<std::int64_t>(i) * 100 * kMs);
  }
  const auto result = est.estimate();
  EXPECT_EQ(result.moves, 0u);
  EXPECT_EQ(result.followed, 0u);
}

TEST(EventStudyEstimator, UnansweredMovesExpireInsteadOfMatchingLate) {
  EventStudyEstimator est(
      EventStudyConfig{.move_cents = 1.0, .follow_window_ns = 1'000 * kMs});
  // A moves at t=1s; B answers at t=5s, past the 1 s window.
  est.observe(50.0, 50.0, 0);
  est.observe(52.0, 50.0, 1'000 * kMs);
  est.observe(52.0, 50.0, 1'500 * kMs);
  est.observe(52.0, 52.0, 5'000 * kMs);
  const auto result = est.estimate();
  EXPECT_EQ(result.moves, 1u);
  EXPECT_EQ(result.followed, 0u);
}

TEST(EventStudyEstimator, EvenFollowCountUsesTheAverageOfTheTwoMiddles) {
  // Two followed moves with delays 400 ms and 800 ms: the median is their
  // average (600 ms), not the upper one.
  EventStudyEstimator est(
      EventStudyConfig{.move_cents = 1.0, .follow_window_ns = 5'000 * kMs});
  // Move A at t=0, followed at t=400ms.
  est.observe(50.0, 50.0, 0);
  est.observe(52.0, 50.0, 100 * kMs);
  est.observe(52.0, 52.0, 500 * kMs);       // follow of A, 400 ms
  // Move B at t=1s, followed at t=1.8s.
  est.observe(54.0, 52.0, 1'000 * kMs);
  est.observe(54.0, 54.0, 1'800 * kMs);     // follow of B, 800 ms
  const auto r = est.estimate();
  EXPECT_EQ(r.followed, 2u);
  EXPECT_NEAR(r.median_follow_seconds, 0.6, 1e-9);
}

TEST(EventStudyEstimator, TooFewSamplesIsSafe) {
  EventStudyEstimator est;
  EXPECT_EQ(est.estimate().moves, 0u);
  est.observe(50.0, 45.0, 0);
  EXPECT_EQ(est.estimate().moves, 0u);
}
