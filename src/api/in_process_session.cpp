#include "api/in_process_session.h"

namespace basis::api {

void InProcessSession::subscribe(const std::string& event_id,
                                 const std::string& field, Handler handler) {
  const std::lock_guard<std::mutex> lock(mutex_);
  if (stopped_) return;
  handlers_[topic_key(event_id, field)].push_back(std::move(handler));
}

void InProcessSession::stop() {
  const std::lock_guard<std::mutex> lock(mutex_);
  stopped_ = true;
  handlers_.clear();
}

void InProcessSession::publish(const Update& update) {
  std::vector<Handler> to_call;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) return;
    const auto it = handlers_.find(topic_key(update.event_id, update.field));
    if (it == handlers_.end()) return;
    to_call = it->second;  // copy: handlers run without the lock held
  }
  for (const auto& handler : to_call) handler(update);
}

}  // namespace basis::api
