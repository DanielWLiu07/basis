#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "api/subscription.h"

namespace basis::api {

// Fan-out session for many subscribers, built on the property that makes
// market data different from an event log: a consumer that falls behind
// wants the CURRENT price, not a backlog of stale ones. So each subscriber
// holds one slot per topic instead of a queue, and a second update to a
// topic the subscriber has not drained yet overwrites the first. That is
// conflation, and it is what lets a slow consumer stay correct instead of
// merely being tolerated.
//
// Two properties follow, and both are the point:
//
//   Publishers never wait on consumers. publish() writes into each
//   interested subscriber's slot and returns; it never runs a handler.
//   InProcessSession, by contrast, invokes every handler inline on the
//   publisher's thread, so one slow subscriber stalls the publisher and
//   every subscriber behind it.
//
//   Memory is bounded by subscribers x topics, not by publish rate or by
//   how far behind a consumer falls. An unbounded queue per subscriber
//   would grow without limit under exactly the conditions (a burst, a
//   stalled consumer) where growing is least affordable.
//
// The cost is deliberate and stated: intermediate values are dropped for a
// subscriber that cannot keep up. That is correct for quotes and wrong for
// anything a consumer must see every instance of (fills, trade prints).
// Such a consumer wants a queue, and BoundedQueue in core/ is that.
//
// Thread-safe. Each subscriber has its own mutex, so publishers contend
// per subscriber rather than on one global lock, and a subscriber draining
// its own slots blocks only publishers touching that same subscriber.
class ConflatingSession final : public Session {
 public:
  // Opaque handle to one subscriber's mailbox. Subscribers drain their own.
  using SubscriberId = std::size_t;

  struct Stats {
    std::uint64_t published = 0;   // publish() calls
    std::uint64_t queued = 0;      // (update, subscriber) pairs slotted
    std::uint64_t delivered = 0;   // handler invocations
    std::uint64_t conflated = 0;   // values overwritten before delivery
    // Entitlement accounting. Denials and withheld values are counted
    // rather than merely prevented, because "nobody was denied anything"
    // is a claim an audit will ask you to evidence.
    std::uint64_t subscriptions_denied = 0;
    std::uint64_t withheld = 0;     // values not delivered, not entitled
    std::uint64_t revocations = 0;
  };

  // Market data is licensed, and who may see which topic is enforced
  // rather than assumed. Two modes, because a session that has never
  // heard of entitlements should not silently behave as if everything is
  // permitted OR as if nothing is:
  //
  //   Open        no entitlement concept; every subscriber may see every
  //               topic. What the in-process replay path wants.
  //   Restricted  default-deny. A subscriber sees a topic only if it has
  //               been granted, which is the posture a real deployment
  //               runs in, because fail-open on a licensing control is
  //               the failure that ends up in front of a regulator.
  enum class Entitlements : std::uint8_t { Open, Restricted };

  void set_entitlements(Entitlements mode);

  // Grant or revoke one (subscriber, topic) pair. Order-independent with
  // respect to subscribe_for: a subscription made before its grant lies
  // dormant and starts delivering when the grant lands, rather than
  // silently never working. (A production API would answer a subscribe
  // with a status instead; this one returns void, so the failure has to
  // be recoverable rather than fatal.)
  //
  // Revocation takes effect immediately, in three places, because
  // "undelivered" is not the same as "not held": it purges what is
  // already slotted, publish() stops storing anything further, and
  // delivery re-checks so a publish racing the revoke cannot slip through
  // behind it.
  void grant(SubscriberId id, const std::string& event_id,
             const std::string& field);
  void revoke(SubscriberId id, const std::string& event_id,
              const std::string& field);

  ConflatingSession() = default;

  // Session interface: subscribes an implicit per-call subscriber, so
  // existing single-consumer code keeps working. Multi-subscriber callers
  // want add_subscriber + subscribe_for + drain.
  void subscribe(const std::string& event_id, const std::string& field,
                 Handler handler) override;
  void stop() override;

  // Registers a subscriber and returns its id. Ids are stable for the
  // session's lifetime; there is no unsubscribe, because a market-data
  // session's subscriber set is set up once and torn down with the session.
  SubscriberId add_subscriber();

  // Subscribing delivers the topic's current value on the next drain, if
  // one has ever been published: a consumer joining a quiet market gets
  // the present image instead of a blank screen until the next tick. The
  // seam is the interesting part. Roster insertion, handler registration
  // and the snapshot all happen under the same lock publish() takes, so a
  // concurrent publish is ordered strictly before or after the join: the
  // joiner cannot miss an update, and cannot be handed a snapshot that is
  // older than a value already slotted for it.
  void subscribe_for(SubscriberId id, const std::string& event_id,
                     const std::string& field, Handler handler);

  // Engine side. Slots the update for every subscriber of the topic and
  // returns. Never invokes a handler, never blocks on a consumer.
  void publish(const Update& update);

  // Consumer side: delivers each topic's latest value to this subscriber's
  // handlers and clears its pending set. Call it from the subscriber's own
  // thread. Returns the number of values delivered (topics, not updates:
  // conflated updates were already collapsed). Handlers run outside the
  // lock, so a handler may publish or drain without deadlocking.
  std::size_t drain(SubscriberId id);

  Stats stats() const;

 private:
  // Topics are interned to a dense id once, so publish() does a hash lookup
  // per call and an integer index per subscriber rather than hashing a
  // composed string per subscriber.
  using TopicId = std::size_t;

  static std::string topic_key(const std::string& event_id,
                               const std::string& field) {
    return event_id + '\n' + field;  // neither may contain '\n'
  }

  TopicId intern_topic(const std::string& key);

  struct Subscriber {
    std::mutex mutex;
    // Latest value per topic this subscriber cares about, plus the topics
    // whose value has not been delivered yet. `pending` is a list rather
    // than a scan of `latest`, so drain costs what changed, not what is
    // subscribed.
    std::unordered_map<TopicId, Update> latest;
    std::vector<TopicId> pending;
    std::unordered_map<TopicId, std::vector<Handler>> handlers;
    // Topics this subscriber is licensed to see. Only consulted in
    // Restricted mode; empty means "nothing" there, and means nothing at
    // all in Open mode.
    std::unordered_set<TopicId> entitled;
    std::uint64_t delivered = 0;
    std::uint64_t conflated = 0;
    // Per-subscriber, summed in stats() like the two above. A single
    // shared counter would be written from drain() under the
    // subscriber's own lock and from revoke() under the registry lock,
    // which is two threads mutating one object with no lock in common.
    std::uint64_t withheld = 0;
  };

  // Slots `update` for `sub`, marking the topic pending and counting a
  // conflation when it supersedes a value nobody has read. Caller holds
  // registry_mutex_; this takes the subscriber's lock. Returns false when
  // the subscriber is not entitled to the topic, in which case nothing is
  // stored: an unentitled subscriber must not merely be undelivered, it
  // must not be holding the data at all.
  bool slot_locked(Subscriber& sub, TopicId topic, const Update& update);

  mutable std::mutex registry_mutex_;  // guards topic map, roster, cache
  std::unordered_map<std::string, TopicId> topic_ids_;
  // Last published value per topic: the image a late joiner starts from.
  // One entry per topic, so it is bounded by the topic set and not by
  // subscriber count or publish rate.
  std::unordered_map<TopicId, Update> last_value_;
  std::vector<std::vector<SubscriberId>> topic_subscribers_;
  // unique_ptr because a Subscriber holds a mutex: the vector must be able
  // to grow without moving live subscribers.
  std::vector<std::unique_ptr<Subscriber>> subscribers_;
  bool stopped_ = false;
  // Atomic because drain() reads it holding only the subscriber's own
  // lock, so a set_entitlements() concurrent with a delivery would
  // otherwise be a race on a plain enum. (publish() does hold
  // registry_mutex_ across its fan-out, so this is a correctness
  // requirement rather than a contention one.)
  std::atomic<Entitlements> entitlements_{Entitlements::Open};
  std::uint64_t published_ = 0;
  std::uint64_t queued_ = 0;
  std::uint64_t denied_ = 0;
  std::uint64_t revocations_ = 0;

  // Caller holds `sub`'s own mutex. Open mode permits everything.
  bool permitted_locked(const Subscriber& sub, TopicId topic) const;
};

}  // namespace basis::api
