#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
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
  };

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
    std::uint64_t delivered = 0;
    std::uint64_t conflated = 0;
  };

  mutable std::mutex registry_mutex_;  // guards topic map and the roster
  std::unordered_map<std::string, TopicId> topic_ids_;
  std::vector<std::vector<SubscriberId>> topic_subscribers_;
  // unique_ptr because a Subscriber holds a mutex: the vector must be able
  // to grow without moving live subscribers.
  std::vector<std::unique_ptr<Subscriber>> subscribers_;
  bool stopped_ = false;
  std::uint64_t published_ = 0;
  std::uint64_t queued_ = 0;
};

}  // namespace basis::api
