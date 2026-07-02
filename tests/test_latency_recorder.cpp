#include <gtest/gtest.h>

#include "bench/latency_recorder.h"

using basis::bench::LatencyRecorder;

TEST(LatencyRecorder, EmptyReportIsZeroes) {
  LatencyRecorder r;
  const auto rep = r.report();
  EXPECT_EQ(rep.count, 0u);
  EXPECT_EQ(rep.p99_ns, 0);
}

TEST(LatencyRecorder, NearestRankPercentiles) {
  LatencyRecorder r;
  for (int i = 100; i >= 1; --i) r.record(i * 10);  // 10..1000, any order
  const auto rep = r.report();
  EXPECT_EQ(rep.count, 100u);
  EXPECT_EQ(rep.min_ns, 10);
  EXPECT_EQ(rep.max_ns, 1000);
  EXPECT_EQ(rep.p50_ns, 500);
  EXPECT_EQ(rep.p90_ns, 900);
  EXPECT_EQ(rep.p99_ns, 990);
  EXPECT_DOUBLE_EQ(rep.mean_ns, 505.0);
}

TEST(LatencyRecorder, SingleSampleIsEveryPercentile) {
  LatencyRecorder r;
  r.record(42);
  const auto rep = r.report();
  EXPECT_EQ(rep.min_ns, 42);
  EXPECT_EQ(rep.p50_ns, 42);
  EXPECT_EQ(rep.p99_ns, 42);
  EXPECT_EQ(rep.max_ns, 42);
}
