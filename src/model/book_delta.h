#pragma once

#include <cstdint>
#include <string>

#include "model/types.h"

namespace basis::model {

// One price-level update on one venue's book. Prediction-market contracts
// trade in cents (1..99c), which is also the implied probability.
struct BookDelta {
  Venue         venue       = Venue::Kalshi;
  std::string   market;             // venue-native market id
  Side          side        = Side::Bid;
  int           price_cents = 0;    // 1..99
  std::int64_t  size        = 0;    // contracts resting at this level after update
  std::uint64_t seq         = 0;    // venue sequence number (gap detection)
  std::int64_t  ts_ns       = 0;    // ingest timestamp, ns since epoch
};

}  // namespace basis::model
