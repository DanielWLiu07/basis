#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "api/in_process_session.h"

using basis::api::InProcessSession;
using basis::api::Update;

TEST(InProcessSession, DeliversToMatchingTopicOnly) {
  InProcessSession s;
  std::vector<double> basis_values;
  int other_calls = 0;

  s.subscribe("fed-cut-2026-09", "basis",
              [&](const Update& u) { basis_values.push_back(u.value); });
  s.subscribe("fed-cut-2026-09", "kalshi_mid",
              [&](const Update&) { ++other_calls; });
  s.subscribe("some-other-event", "basis",
              [&](const Update&) { ++other_calls; });

  s.publish({.event_id = "fed-cut-2026-09", .field = "basis",
             .value = 2.5, .ts_ns = 1});
  s.publish({.event_id = "fed-cut-2026-09", .field = "basis",
             .value = 3.0, .ts_ns = 2});

  ASSERT_EQ(basis_values.size(), 2u);
  EXPECT_DOUBLE_EQ(basis_values[0], 2.5);
  EXPECT_DOUBLE_EQ(basis_values[1], 3.0);
  EXPECT_EQ(other_calls, 0);
}

TEST(InProcessSession, MultipleHandlersOnOneTopicAllFire) {
  InProcessSession s;
  int a = 0;
  int b = 0;
  s.subscribe("e", "f", [&](const Update&) { ++a; });
  s.subscribe("e", "f", [&](const Update&) { ++b; });
  s.publish({.event_id = "e", .field = "f"});
  EXPECT_EQ(a, 1);
  EXPECT_EQ(b, 1);
}

TEST(InProcessSession, StopDropsDeliveryAndNewSubscriptions) {
  InProcessSession s;
  int calls = 0;
  s.subscribe("e", "f", [&](const Update&) { ++calls; });
  s.stop();
  s.publish({.event_id = "e", .field = "f"});
  s.subscribe("e", "f", [&](const Update&) { ++calls; });
  s.publish({.event_id = "e", .field = "f"});
  EXPECT_EQ(calls, 0);
}

TEST(InProcessSession, HandlerMaySubscribeWithoutDeadlock) {
  InProcessSession s;
  int late_calls = 0;
  s.subscribe("e", "f", [&](const Update&) {
    s.subscribe("e", "g", [&](const Update&) { ++late_calls; });
  });
  s.publish({.event_id = "e", .field = "f"});
  s.publish({.event_id = "e", .field = "g"});
  EXPECT_EQ(late_calls, 1);
}
