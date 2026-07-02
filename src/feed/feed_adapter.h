#pragma once

#include <functional>

#include "model/book_delta.h"

namespace basis::feed {

// A FeedAdapter connects to one venue, subscribes to a set of markets, and
// emits canonical model::BookDelta events through a sink. It owns reconnect,
// heartbeat, and sequence-gap recovery; consumers never see venue JSON.
//
// Implementations (Phase 1/2): KalshiFeed, PolymarketFeed.
class FeedAdapter {
 public:
  using DeltaSink = std::function<void(const model::BookDelta&)>;

  virtual ~FeedAdapter() = default;

  virtual model::Venue venue() const = 0;
  virtual void set_sink(DeltaSink sink) = 0;
  virtual void start() = 0;  // connect + subscribe (non-blocking)
  virtual void stop() = 0;
};

}  // namespace basis::feed
