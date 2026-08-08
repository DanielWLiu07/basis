#include "api/conflating_session.h"

#include <utility>

namespace basis::api {

ConflatingSession::TopicId ConflatingSession::intern_topic(
    const std::string& key) {
  const auto it = topic_ids_.find(key);
  if (it != topic_ids_.end()) return it->second;
  const TopicId id = topic_subscribers_.size();
  topic_ids_.emplace(key, id);
  topic_subscribers_.emplace_back();
  return id;
}

ConflatingSession::SubscriberId ConflatingSession::add_subscriber() {
  const std::lock_guard<std::mutex> lock(registry_mutex_);
  subscribers_.push_back(std::make_unique<Subscriber>());
  return subscribers_.size() - 1;
}

void ConflatingSession::slot_locked(Subscriber& sub, TopicId topic,
                                    const Update& update) {
  const std::lock_guard<std::mutex> sub_lock(sub.mutex);
  const auto [slot, inserted] = sub.latest.insert_or_assign(topic, update);
  (void)slot;
  bool pending = false;
  if (!inserted) {
    for (TopicId t : sub.pending) {
      if (t == topic) { pending = true; break; }
    }
  }
  if (pending) {
    ++sub.conflated;  // superseded before anyone read it
  } else {
    sub.pending.push_back(topic);
  }
}

void ConflatingSession::subscribe_for(SubscriberId id,
                                      const std::string& event_id,
                                      const std::string& field,
                                      Handler handler) {
  // Everything here runs under registry_mutex_, the same lock publish()
  // holds while it fans out. That is what makes the join atomic against
  // the stream: a publish is ordered wholly before this call (so the
  // snapshot below carries its value) or wholly after (so the roster
  // already contains this subscriber and the normal path delivers it).
  // Registering the handler outside this lock would also lose values: a
  // publish could slot one while drain() finds no handler for the topic,
  // skips it, and clears pending.
  const std::lock_guard<std::mutex> lock(registry_mutex_);
  if (stopped_ || id >= subscribers_.size()) return;
  const TopicId topic = intern_topic(topic_key(event_id, field));
  auto& roster = topic_subscribers_[topic];
  // A subscriber appears once per topic however many handlers it adds,
  // so publish() writes its slot once.
  bool listed = false;
  for (SubscriberId existing : roster) {
    if (existing == id) { listed = true; break; }
  }
  if (!listed) roster.push_back(id);
  Subscriber& sub = *subscribers_[id];
  {
    const std::lock_guard<std::mutex> sub_lock(sub.mutex);
    sub.handlers[topic].push_back(std::move(handler));
  }
  // Snapshot: hand the joiner the current image so it is useful before
  // the next tick, which on a quiet market may be minutes away.
  const auto cached = last_value_.find(topic);
  if (cached != last_value_.end()) slot_locked(sub, topic, cached->second);
}

void ConflatingSession::subscribe(const std::string& event_id,
                                  const std::string& field, Handler handler) {
  // Session-interface path: one implicit subscriber per call keeps
  // single-consumer callers working unchanged.
  subscribe_for(add_subscriber(), event_id, field, std::move(handler));
}

void ConflatingSession::publish(const Update& update) {
  // Snapshot the roster under the registry lock, then touch each
  // subscriber under its own lock. A publisher therefore holds the shared
  // lock for a hash lookup, not for the fan-out.
  std::vector<Subscriber*> targets;
  {
    const std::lock_guard<std::mutex> lock(registry_mutex_);
    if (stopped_) return;
    ++published_;
    // Intern rather than look up: a topic with no subscribers yet still
    // needs its image recorded, because that is exactly the value a late
    // joiner will ask for. Topics come from the contract registry, so the
    // interned set is bounded by the configured markets.
    const TopicId topic = intern_topic(topic_key(update.event_id, update.field));
    // Remember the image so a subscriber joining later starts from it.
    last_value_.insert_or_assign(topic, update);
    targets.reserve(topic_subscribers_[topic].size());
    for (SubscriberId id : topic_subscribers_[topic]) {
      targets.push_back(subscribers_[id].get());
    }
    queued_ += targets.size();
    for (Subscriber* sub : targets) slot_locked(*sub, topic, update);
  }
}

std::size_t ConflatingSession::drain(SubscriberId id) {
  Subscriber* sub = nullptr;
  {
    const std::lock_guard<std::mutex> lock(registry_mutex_);
    if (id >= subscribers_.size()) return 0;
    sub = subscribers_[id].get();
  }

  // Take the pending set and its values, then run handlers with no lock
  // held: a handler may publish, drain, or block without stalling anyone
  // else's publish path.
  std::vector<std::pair<Update, std::vector<Handler>>> work;
  {
    const std::lock_guard<std::mutex> lock(sub->mutex);
    work.reserve(sub->pending.size());
    for (TopicId topic : sub->pending) {
      const auto value = sub->latest.find(topic);
      const auto handlers = sub->handlers.find(topic);
      if (value == sub->latest.end() || handlers == sub->handlers.end()) continue;
      work.emplace_back(value->second, handlers->second);
    }
    sub->pending.clear();
    sub->delivered += work.size();
  }
  for (const auto& [update, handlers] : work) {
    for (const auto& handler : handlers) handler(update);
  }
  return work.size();
}

void ConflatingSession::stop() {
  const std::lock_guard<std::mutex> lock(registry_mutex_);
  stopped_ = true;
}

ConflatingSession::Stats ConflatingSession::stats() const {
  const std::lock_guard<std::mutex> lock(registry_mutex_);
  Stats s;
  s.published = published_;
  s.queued = queued_;
  for (const auto& sub : subscribers_) {
    const std::lock_guard<std::mutex> sub_lock(sub->mutex);
    s.delivered += sub->delivered;
    s.conflated += sub->conflated;
  }
  return s;
}

}  // namespace basis::api
