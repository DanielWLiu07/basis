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

  // The publisher can finish before the slow consumer's thread is ever
  // scheduled - that is the whole point of the decoupling, and it is why
  // asserting on slow_calls right here would be a race rather than a
  // check. Wait for the consumer to make progress, bounded so a genuine
  // failure to deliver still fails the test instead of hanging.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (slow_calls.load() == 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const int slow_seen = slow_calls.load();
  running = false;
  slow_thread.join();

  // 2,000 publishes against a handler that sleeps 20 ms each call: if the
  // publisher were coupled to the consumer this would take many seconds.
  EXPECT_LT(publish_ms, 2'000.0);
  EXPECT_DOUBLE_EQ(fast_last.load(), 2'000.0);  // fast one is current
  EXPECT_GT(slow_seen, 0);        // the slow one does receive, eventually
  EXPECT_LT(slow_seen, 2'000);    // having skipped the stale middle
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

TEST(ConflatingSession, LateJoinerGetsTheCurrentImageWithoutWaitingForATick) {
  ConflatingSession s;
  // The market prints, then goes quiet.
  s.publish(tick("fed", "mid", 47.0));
  s.publish(tick("fed", "mid", 48.5));

  // A consumer arrives after all of that. Without a snapshot it would see
  // nothing until the next print, which on a quiet book can be minutes.
  const auto late = s.add_subscriber();
  double seen = 0.0;
  int calls = 0;
  s.subscribe_for(late, "fed", "mid", [&](const Update& u) {
    seen = u.value;
    ++calls;
  });

  EXPECT_EQ(s.drain(late), 1u);
  EXPECT_EQ(calls, 1);
  EXPECT_DOUBLE_EQ(seen, 48.5);  // the current image, not the first print

  // And it is a snapshot, not a replay: the superseded 47.0 is not
  // delivered, and the stream continues normally afterwards.
  s.publish(tick("fed", "mid", 49.0));
  EXPECT_EQ(s.drain(late), 1u);
  EXPECT_DOUBLE_EQ(seen, 49.0);
  EXPECT_EQ(calls, 2);
}

TEST(ConflatingSession, SubscribingToASilentTopicDeliversNothing) {
  ConflatingSession s;
  const auto sub = s.add_subscriber();
  int calls = 0;
  s.subscribe_for(sub, "wc26", "mid", [&](const Update&) { ++calls; });
  // Nothing has ever been published on this topic, so there is no image to
  // hand over and the subscriber correctly sees nothing at all.
  EXPECT_EQ(s.drain(sub), 0u);
  EXPECT_EQ(calls, 0);
}

TEST(ConflatingSession, SnapshotDoesNotResurrectAValueAlreadySlotted) {
  ConflatingSession s;
  const auto sub = s.add_subscriber();
  std::vector<double> seen;
  s.subscribe_for(sub, "fed", "mid",
                  [&](const Update& u) { seen.push_back(u.value); });
  s.publish(tick("fed", "mid", 10.0));

  // A second handler on a topic this subscriber already follows must not
  // re-deliver the cached image on top of the value already waiting: the
  // subscriber is not "joining" anything it is not already in.
  s.subscribe_for(sub, "fed", "mid",
                  [&](const Update& u) { seen.push_back(u.value); });
  EXPECT_EQ(s.drain(sub), 1u);   // one topic pending, not two
  ASSERT_EQ(seen.size(), 2u);    // both handlers ran, once each
  EXPECT_DOUBLE_EQ(seen[0], 10.0);
  EXPECT_DOUBLE_EQ(seen[1], 10.0);
}

TEST(ConflatingSession, JoiningDuringAPublishStormNeverMissesTheFinalValue) {
  // The race the atomic join exists for: subscribers arrive while a
  // publisher is running flat out. Each one must end holding the last
  // published value, whichever side of the fan-out its join landed on.
  ConflatingSession s;
  constexpr int kUpdates = 20'000;
  constexpr int kJoiners = 16;

  std::atomic<bool> publishing{true};
  std::thread publisher([&] {
    for (int i = 1; i <= kUpdates; ++i) {
      s.publish(tick("fed", "mid", static_cast<double>(i)));
    }
    publishing = false;
  });

  std::vector<ConflatingSession::SubscriberId> ids;
  std::vector<std::atomic<double>> last(kJoiners);
  std::vector<std::thread> joiners;
  std::mutex ids_mutex;
  for (int j = 0; j < kJoiners; ++j) {
    joiners.emplace_back([&, j] {
      const auto id = s.add_subscriber();
      {
        const std::lock_guard<std::mutex> lock(ids_mutex);
        ids.push_back(id);
      }
      s.subscribe_for(id, "fed", "mid",
                      [&last, j](const Update& u) { last[j] = u.value; });
      while (publishing) s.drain(id);
      s.drain(id);
    });
  }
  publisher.join();
  for (auto& t : joiners) t.join();
  // Final sweep: publishing has stopped, so one more drain settles anyone
  // whose last drain raced the final publish.
  {
    const std::lock_guard<std::mutex> lock(ids_mutex);
    for (auto id : ids) s.drain(id);
  }

  for (int j = 0; j < kJoiners; ++j) {
    EXPECT_DOUBLE_EQ(last[j].load(), static_cast<double>(kUpdates))
        << "joiner " << j << " did not end on the final value";
  }
}

using E = ConflatingSession::Entitlements;

TEST(ConflatingSession, RestrictedModeDeniesUngrantedTopics) {
  ConflatingSession s;
  s.set_entitlements(E::Restricted);
  const auto sub = s.add_subscriber();
  int calls = 0;
  // Default-deny: no grant, so the subscription never takes effect and
  // the publish has nowhere to land.
  s.subscribe_for(sub, "fed", "mid", [&](const Update&) { ++calls; });
  s.publish(tick("fed", "mid", 50.0));
  EXPECT_EQ(s.drain(sub), 0u);
  EXPECT_EQ(calls, 0);
  EXPECT_EQ(s.stats().subscriptions_denied, 1u);
}

TEST(ConflatingSession, GrantedTopicsFlowNormally) {
  ConflatingSession s;
  s.set_entitlements(E::Restricted);
  const auto sub = s.add_subscriber();
  double seen = 0.0;
  s.grant(sub, "fed", "mid");
  s.subscribe_for(sub, "fed", "mid", [&](const Update& u) { seen = u.value; });
  s.publish(tick("fed", "mid", 50.0));
  EXPECT_EQ(s.drain(sub), 1u);
  EXPECT_DOUBLE_EQ(seen, 50.0);
  EXPECT_EQ(s.stats().subscriptions_denied, 0u);
}

TEST(ConflatingSession, EntitlementIsPerSubscriberAndPerTopic) {
  ConflatingSession s;
  s.set_entitlements(E::Restricted);
  const auto a = s.add_subscriber();
  const auto b = s.add_subscriber();
  int a_mid = 0, a_basis = 0, b_mid = 0;
  s.grant(a, "fed", "mid");          // a sees mid but not basis
  s.grant(b, "fed", "basis");        // b sees basis but not mid
  s.subscribe_for(a, "fed", "mid", [&](const Update&) { ++a_mid; });
  s.subscribe_for(a, "fed", "basis", [&](const Update&) { ++a_basis; });
  s.subscribe_for(b, "fed", "mid", [&](const Update&) { ++b_mid; });

  s.publish(tick("fed", "mid", 1.0));
  s.publish(tick("fed", "basis", 2.0));
  s.drain(a);
  s.drain(b);
  EXPECT_EQ(a_mid, 1);
  EXPECT_EQ(a_basis, 0);  // licensed for mid only
  EXPECT_EQ(b_mid, 0);    // licensed for basis only
}

TEST(ConflatingSession, RevocationDropsAValueAlreadyWaitingInTheSlot) {
  // The case that makes revocation more than a flag: the value was
  // published while the subscriber was still entitled and is already
  // sitting in its slot. Revoking must reach in and drop it, because
  // delivering it afterwards is delivering data the subscriber is no
  // longer licensed to see.
  ConflatingSession s;
  s.set_entitlements(E::Restricted);
  const auto sub = s.add_subscriber();
  int calls = 0;
  s.grant(sub, "fed", "mid");
  s.subscribe_for(sub, "fed", "mid", [&](const Update&) { ++calls; });
  s.publish(tick("fed", "mid", 50.0));   // slotted, not yet drained

  s.revoke(sub, "fed", "mid");
  EXPECT_EQ(s.drain(sub), 0u);
  EXPECT_EQ(calls, 0);
  EXPECT_EQ(s.stats().revocations, 1u);
  EXPECT_GT(s.stats().withheld, 0u);
}

TEST(ConflatingSession, APublishAfterRevocationCannotSlipThrough) {
  // The other half of the window: a publish that lands after the revoke
  // must not be delivered either, even though the roster still lists the
  // subscriber for that topic. Delivery re-checks, so it is withheld.
  ConflatingSession s;
  s.set_entitlements(E::Restricted);
  const auto sub = s.add_subscriber();
  int calls = 0;
  s.grant(sub, "fed", "mid");
  s.subscribe_for(sub, "fed", "mid", [&](const Update&) { ++calls; });
  s.revoke(sub, "fed", "mid");

  s.publish(tick("fed", "mid", 99.0));
  EXPECT_EQ(s.drain(sub), 0u);
  EXPECT_EQ(calls, 0);

  // And a re-grant restores delivery without needing a resubscribe.
  s.grant(sub, "fed", "mid");
  s.publish(tick("fed", "mid", 101.0));
  EXPECT_EQ(s.drain(sub), 1u);
  EXPECT_EQ(calls, 1);
}

TEST(ConflatingSession, OpenModeIgnoresEntitlementsEntirely) {
  // The replay path never grants anything; it must keep working.
  ConflatingSession s;  // Open by default
  const auto sub = s.add_subscriber();
  int calls = 0;
  s.subscribe_for(sub, "fed", "mid", [&](const Update&) { ++calls; });
  s.publish(tick("fed", "mid", 7.0));
  EXPECT_EQ(s.drain(sub), 1u);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(s.stats().subscriptions_denied, 0u);
  EXPECT_EQ(s.stats().withheld, 0u);
}

TEST(ConflatingSession, GrantAfterSubscribeActivatesTheSubscription) {
  // The ordering trap: a subscription made before its grant used to be
  // dropped outright, so the later grant did nothing and the subscriber
  // silently received nothing forever.
  ConflatingSession s;
  s.set_entitlements(E::Restricted);
  const auto sub = s.add_subscriber();
  double seen = 0.0;
  s.subscribe_for(sub, "fed", "mid", [&](const Update& u) { seen = u.value; });
  s.publish(tick("fed", "mid", 1.0));
  EXPECT_EQ(s.drain(sub), 0u);           // dormant, correctly delivering nothing
  EXPECT_EQ(s.stats().subscriptions_denied, 1u);

  s.grant(sub, "fed", "mid");            // no resubscribe
  s.publish(tick("fed", "mid", 2.0));
  EXPECT_EQ(s.drain(sub), 1u);
  EXPECT_DOUBLE_EQ(seen, 2.0);
}

TEST(ConflatingSession, UnentitledDataIsNeverStoredNotMerelyUndelivered) {
  // Withholding delivery is not enough for a licensing control: if the
  // publish still writes the value into the revoked subscriber's slot, it
  // is holding licensed data it may not see until re-grant or teardown.
  // Re-granting must therefore NOT surface anything published while the
  // entitlement was gone.
  ConflatingSession s;
  s.set_entitlements(E::Restricted);
  const auto sub = s.add_subscriber();
  std::vector<double> seen;
  s.grant(sub, "fed", "mid");
  s.subscribe_for(sub, "fed", "mid",
                  [&](const Update& u) { seen.push_back(u.value); });
  s.revoke(sub, "fed", "mid");

  s.publish(tick("fed", "mid", 99.0));   // must not be retained anywhere
  EXPECT_EQ(s.drain(sub), 0u);

  s.grant(sub, "fed", "mid");
  // Nothing to deliver: the 99.0 was never stored, so re-granting cannot
  // leak it. Only a value published after the re-grant arrives.
  EXPECT_EQ(s.drain(sub), 0u);
  EXPECT_TRUE(seen.empty());
  s.publish(tick("fed", "mid", 100.0));
  EXPECT_EQ(s.drain(sub), 1u);
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_DOUBLE_EQ(seen[0], 100.0);
}

TEST(ConflatingSession, RevokingAfterDeliveryReportsNoWithholding) {
  // A value already delivered still sits in `latest` as the conflation
  // slot. Counting its removal as a withholding would report a
  // withholding that never happened, in a number offered as evidence.
  ConflatingSession s;
  s.set_entitlements(E::Restricted);
  const auto sub = s.add_subscriber();
  s.grant(sub, "fed", "mid");
  s.subscribe_for(sub, "fed", "mid", [](const Update&) {});
  s.publish(tick("fed", "mid", 5.0));
  ASSERT_EQ(s.drain(sub), 1u);           // delivered legitimately

  s.revoke(sub, "fed", "mid");
  EXPECT_EQ(s.stats().withheld, 0u);     // nothing was actually withheld
  EXPECT_EQ(s.stats().revocations, 1u);
}

TEST(ConflatingSession, EntitlementCountersAreExactUnderConcurrentDrains) {
  // The counters are mutated from drain(), which holds only a
  // subscriber's own lock. A single shared counter would be a data race
  // across concurrent drainers with no lock in common, and would lose
  // increments silently. Restricted mode is what reaches that path, so
  // this is the multithreaded test that exercises it.
  ConflatingSession s;
  s.set_entitlements(E::Restricted);
  constexpr int kSubs = 8;
  constexpr int kUpdates = 2'000;
  std::vector<ConflatingSession::SubscriberId> ids;
  for (int i = 0; i < kSubs; ++i) {
    const auto id = s.add_subscriber();
    ids.push_back(id);
    // Half entitled, half not: the unentitled half drives the withheld
    // path on every publish.
    if (i % 2 == 0) s.grant(id, "fed", "mid");
    s.subscribe_for(id, "fed", "mid", [](const Update&) {});
  }

  std::atomic<bool> running{true};
  std::vector<std::thread> drainers;
  for (int i = 0; i < kSubs; ++i) {
    drainers.emplace_back([&, i] {
      while (running) s.drain(ids[i]);
      s.drain(ids[i]);
    });
  }
  for (int u = 0; u < kUpdates; ++u) {
    s.publish(tick("fed", "mid", static_cast<double>(u)));
  }
  running = false;
  for (auto& t : drainers) t.join();

  const auto st = s.stats();
  EXPECT_EQ(st.published, static_cast<std::uint64_t>(kUpdates));
  // Four unentitled subscribers, one skipped slot each per publish, and
  // nothing stored for them: exact, not approximate.
  EXPECT_EQ(st.withheld, static_cast<std::uint64_t>(kUpdates) * 4);
  // Only entitled subscribers were ever queued, and every queued value is
  // accounted for as delivered or conflated.
  EXPECT_EQ(st.queued, static_cast<std::uint64_t>(kUpdates) * 4);
  EXPECT_EQ(st.queued, st.delivered + st.conflated);
}
