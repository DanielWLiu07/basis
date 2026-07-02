#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "api/subscription.h"

namespace basis::api {

// Synchronous Session for the in-process engine: publish() delivers on the
// caller's thread to every handler subscribed to that (event, field) topic.
// Thread-safe; handlers run outside the lock, so a handler may subscribe
// without deadlocking. A queued asynchronous session arrives with the live
// feeds (Phase 6), behind this same interface.
class InProcessSession final : public Session {
 public:
  void subscribe(const std::string& event_id, const std::string& field,
                 Handler handler) override;
  void stop() override;

  // Engine side. Updates published after stop() are dropped.
  void publish(const Update& update);

 private:
  static std::string topic_key(const std::string& event_id,
                               const std::string& field) {
    // '\n' cannot appear in an event id or field name, so the joined key
    // cannot collide across topics.
    return event_id + '\n' + field;
  }

  mutable std::mutex mutex_;
  bool stopped_ = false;
  std::unordered_map<std::string, std::vector<Handler>> handlers_;
};

}  // namespace basis::api
