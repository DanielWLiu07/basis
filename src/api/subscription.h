#pragma once

#include <functional>
#include <string>

#include "model/types.h"

namespace basis::api {

// BLPAPI-style consumer interface: open a Session, subscribe to (event, field)
// topics, and receive push updates. Mirrors Bloomberg's subscription model so
// the engine reads like infra a Bloomberg consumer would recognize.
struct Update {
  std::string event_id;
  std::string field;   // e.g. "kalshi_mid", "poly_mid", "basis", "lead_secs"
  double      value = 0.0;
  std::int64_t ts_ns = 0;
};

class Session {
 public:
  using Handler = std::function<void(const Update&)>;

  virtual ~Session() = default;

  virtual void subscribe(const std::string& event_id,
                         const std::string& field,
                         Handler handler) = 0;
  virtual void stop() = 0;
};

}  // namespace basis::api
