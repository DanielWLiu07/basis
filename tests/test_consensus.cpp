#include <gtest/gtest.h>

#include "analytics/consensus.h"

using basis::analytics::EventStudyResult;
using basis::analytics::Leader;
using basis::analytics::lead_consensus;
using basis::analytics::LeadLagResult;

namespace {

// A lead-lag result whose bootstrap interval sits entirely on one side of
// zero (significant) in the given direction, or straddles zero when
// significant is false.
LeadLagResult ll_result(double lead_seconds, bool significant) {
  LeadLagResult r;
  r.lead_seconds = lead_seconds;
  r.resamples = 200;
  if (significant) {
    r.ci_low_seconds = lead_seconds > 0 ? 0.1 : -0.3;
    r.ci_high_seconds = lead_seconds > 0 ? 0.3 : -0.1;
  } else {
    r.ci_low_seconds = -0.2;  // spans zero
    r.ci_high_seconds = 0.2;
  }
  return r;
}

// An event-study result whose forward/reverse follow counts push the
// two-proportion z strongly toward A (+1), B (-1), or neither (0).
EventStudyResult es_result(int leader) {
  EventStudyResult r;
  r.moves = 100;
  r.reverse_moves = 100;
  if (leader > 0) {
    r.followed = 90;
    r.reverse_followed = 30;
  } else if (leader < 0) {
    r.followed = 30;
    r.reverse_followed = 90;
  } else {
    r.followed = 50;
    r.reverse_followed = 50;
  }
  return r;
}

}  // namespace

TEST(LeadConsensus, BothMethodsAgreeOnA) {
  const auto c = lead_consensus(ll_result(0.2, true), es_result(1));
  EXPECT_EQ(c.crosscorr, Leader::A);
  EXPECT_EQ(c.event_study, Leader::A);
  EXPECT_TRUE(c.agree());
  EXPECT_FALSE(c.conflict());
  EXPECT_EQ(c.leader(), Leader::A);
}

TEST(LeadConsensus, BothMethodsAgreeOnB) {
  const auto c = lead_consensus(ll_result(-0.2, true), es_result(-1));
  EXPECT_TRUE(c.agree());
  EXPECT_EQ(c.leader(), Leader::B);
}

TEST(LeadConsensus, MethodsConflictResolvesToNoLeader) {
  // Cross-correlation says A, the event study says B: a real conflict.
  const auto c = lead_consensus(ll_result(0.2, true), es_result(-1));
  EXPECT_EQ(c.crosscorr, Leader::A);
  EXPECT_EQ(c.event_study, Leader::B);
  EXPECT_FALSE(c.agree());
  EXPECT_TRUE(c.conflict());
  EXPECT_EQ(c.leader(), Leader::None);
}

TEST(LeadConsensus, OneMethodSilentIsNeitherAgreementNorConflict) {
  // Cross-correlation resolves A; the event study is inconclusive (z ~ 0).
  const auto c = lead_consensus(ll_result(0.2, true), es_result(0));
  EXPECT_EQ(c.crosscorr, Leader::A);
  EXPECT_EQ(c.event_study, Leader::None);
  EXPECT_FALSE(c.agree());
  EXPECT_FALSE(c.conflict());
  EXPECT_EQ(c.leader(), Leader::None);
}

TEST(LeadConsensus, NeitherMethodResolvesADirection) {
  const auto c = lead_consensus(ll_result(0.2, false), es_result(0));
  EXPECT_EQ(c.crosscorr, Leader::None);
  EXPECT_EQ(c.event_study, Leader::None);
  EXPECT_FALSE(c.agree());
  EXPECT_FALSE(c.conflict());
}
