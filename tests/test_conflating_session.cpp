#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "api/conflating_session.h"

using basis::api::ConflatingSession;
using basis::api::Update;

namespace {

Update tick(const char* event, const char* field, double value,
            std::int64_t ts = 0) {
  return Update{event, field, value, ts};
}

}  // namespace

TEST(ConflatingSession, DeliversTheLatestValueNotEveryValue) {
  ConflatingSession s;
  const auto sub = s.add_subscriber();
  std::vector<double> seen;
  s.subscribe_for(sub, "fed", "mid",
                  [&](const Update& u) { seen.push_back(u.value); });

  // Three publishes, no drain in between: the subscriber was never given a
  // chance to read the first two, so it sees only the current price.
  s.publish(tick("fed", "mid", 41.0));
  s.publish(tick("fed", "mid", 42.0));
  s.publish(tick("fed", "mid", 43.0));
  EXPECT_EQ(s.drain(sub), 1u);
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_DOUBLE_EQ(seen[0], 43.0);

  const auto st = s.stats();
  EXPECT_EQ(st.published, 3u);
  EXPECT_EQ(st.delivered, 1u);
  EXPECT_EQ(st.conflated, 2u);  // two values superseded before anyone read
}

TEST(ConflatingSession, KeepsUpAndNothingIsConflated) {
  ConflatingSession s;
  const auto sub = s.add_subscriber();
  std::vector<double> seen;
  s.subscribe_for(sub, "fed", "mid",
                  [&](const Update& u) { seen.push_back(u.value); });

  // A consumer that drains after every publish loses nothing: conflation
  // is a consequence of falling behind, not a fixed sampling rate.
  for (int i = 0; i < 5; ++i) {
    s.publish(tick("fed", "mid", static_cast<double>(i)));
    EXPECT_EQ(s.drain(sub), 1u);
  }
  ASSERT_EQ(seen.size(), 5u);
  EXPECT_DOUBLE_EQ(seen.back(), 4.0);
  EXPECT_EQ(s.stats().conflated, 0u);
}

TEST(ConflatingSession, TopicsAreIndependentAndSubscribersIsolated) {
  ConflatingSession s;
  const auto a = s.add_subscriber();
  const auto b = s.add_subscriber();
  double a_mid = 0.0, a_basis = 0.0, b_mid = 0.0;
  s.subscribe_for(a, "fed", "mid", [&](const Update& u) { a_mid = u.value; });
  s.subscribe_for(a, "fed", "basis", [&](const Update& u) { a_basis = u.value; });
  s.subscribe_for(b, "fed", "mid", [&](const Update& u) { b_mid = u.value; });

  s.publish(tick("fed", "mid", 50.0));
  s.publish(tick("fed", "basis", -1.5));
  s.publish(tick("wc26", "mid", 12.0));  // nobody subscribes: no-op

  // Draining A delivers both of A's topics and leaves B's slot untouched.
  EXPECT_EQ(s.drain(a), 2u);
  EXPECT_DOUBLE_EQ(a_mid, 50.0);
  EXPECT_DOUBLE_EQ(a_basis, -1.5);
  EXPECT_DOUBLE_EQ(b_mid, 0.0);

  EXPECT_EQ(s.drain(b), 1u);
  EXPECT_DOUBLE_EQ(b_mid, 50.0);
  EXPECT_EQ(s.drain(a), 0u);  // nothing new
}

TEST(ConflatingSession, MemoryIsBoundedBySubscribersTimesTopics) {
  // The property that makes this usable at fan-out scale: publishing a
  // million updates to a consumer that never drains costs one slot, not a
  // million. A queue-per-subscriber design would be a million deep here.
  ConflatingSession s;
  const auto sub = s.add_subscriber();
  int calls = 0;
  s.subscribe_for(sub, "fed", "mid", [&](const Update&) { ++calls; });
  for (int i = 0; i < 100'000; ++i) {
    s.publish(tick("fed", "mid", static_cast<double>(i)));
  }
  EXPECT_EQ(s.drain(sub), 1u);
  EXPECT_EQ(calls, 1);
  const auto st = s.stats();
  EXPECT_EQ(st.published, 100'000u);
  EXPECT_EQ(st.delivered, 1u);
  EXPECT_EQ(st.conflated, 99'999u);
}

TEST(ConflatingSession, SlowSubscriberDoesNotStallThePublisher) {
  // The headline property. One subscriber holds its handler for a visible
  // amount of time; the publisher must not be waiting on it, and the fast
  // subscriber must keep seeing current values.
  ConflatingSession s;
  const auto slow = s.add_subscriber();
  const auto fast = s.add_subscriber();
  std::atomic<double> fast_last{0.0};
  std::atomic<int> slow_calls{0};
  s.subscribe_for(slow, "fed", "mid", [&](const Update&) {
    ++slow_calls;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  });
  s.subscribe_for(fast, "fed", "mid",
                  [&](const Update& u) { fast_last = u.value; });

  std::atomic<bool> running{true};
  std::thread slow_thread([&] {
    while (running) s.drain(slow);
  });

  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 1; i <= 2'000; ++i) {
    s.publish(tick("fed", "mid", static_cast<double>(i)));
    s.drain(fast);
  }
  const auto publish_ms = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - t0).count();
  running = false;
  slow_thread.join();

  // 2,000 publishes against a handler that sleeps 20 ms each call: if the
  // publisher were coupled to the consumer this would take many seconds.
  EXPECT_LT(publish_ms, 2'000.0);
  EXPECT_DOUBLE_EQ(fast_last.load(), 2'000.0);  // fast one is current
  EXPECT_GT(slow_calls.load(), 0);              // slow one still got values
  EXPECT_LT(slow_calls.load(), 2'000);          // and skipped the stale ones
}

TEST(ConflatingSession, ConcurrentPublishersAndSubscribersStayConsistent) {
  ConflatingSession s;
  constexpr int kSubscribers = 8;
  constexpr int kPerPublisher = 5'000;
  constexpr int kPublishers = 4;
  std::vector<ConflatingSession::SubscriberId> ids;
  std::vector<std::atomic<int>> counts(kSubscribers);
  for (int i = 0; i < kSubscribers; ++i) {
    const auto id = s.add_subscriber();
    ids.push_back(id);
    s.subscribe_for(id, "fed", "mid",
                    [&counts, i](const Update&) { ++counts[i]; });
  }

  std::atomic<bool> running{true};
  std::vector<std::thread> drainers;
  for (int i = 0; i < kSubscribers; ++i) {
    drainers.emplace_back([&, i] {
      while (running) s.drain(ids[i]);
      s.drain(ids[i]);  // final sweep after publishers stop
    });
  }
  std::vector<std::thread> publishers;
  for (int p = 0; p < kPublishers; ++p) {
    publishers.emplace_back([&] {
      for (int i = 0; i < kPerPublisher; ++i) {
        s.publish(tick("fed", "mid", static_cast<double>(i)));
      }
    });
  }
  for (auto& t : publishers) t.join();
  running = false;
  for (auto& t : drainers) t.join();

  const auto st = s.stats();
  EXPECT_EQ(st.published, static_cast<std::uint64_t>(kPublishers * kPerPublisher));
  // Every slotted value is either delivered or conflated: nothing is lost
  // and nothing is invented, whatever the interleaving.
  EXPECT_EQ(st.queued, st.delivered + st.conflated);
  EXPECT_EQ(st.queued,
            static_cast<std::uint64_t>(kPublishers * kPerPublisher * kSubscribers));
}
