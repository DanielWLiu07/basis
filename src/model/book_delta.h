#pragma once

#include <cstdint>
#include <string>

#include "model/types.h"

namespace basis::model {

// How a delta changes a price level. The two venues disagree here, and this
// enum is where that disagreement is absorbed: Kalshi's orderbook_delta
// carries a size *change*, Polymarket's price_change carries the *absolute*
// size at the level.
enum class Action : std::uint8_t {
  Set,    // size = absolute size at the level; 0 removes it
  Add,    // size = signed change to the level; result <= 0 removes it
  Clear,  // drop the whole book (snapshot head, gap recovery); other fields unused
};

// One price-level update on one venue's book. Prediction-market contracts
// trade in cents (1..99c), which is also the implied probability.
struct BookDelta {
  Venue         venue       = Venue::Kalshi;
  std::string   market;             // venue-native market id
  Action        action      = Action::Set;
  Side          side        = Side::Bid;
  int           price_cents = 0;    // 1..99
  std::int64_t  size        = 0;    // meaning depends on action
  std::uint64_t seq         = 0;    // venue sequence number (gap detection)
  std::int64_t  ts_ns       = 0;    // ingest timestamp, ns since epoch
};

constexpr const char* to_string(Action a) {
  switch (a) {
    case Action::Set:   return "set";
    case Action::Add:   return "add";
    case Action::Clear: return "clear";
  }
  return "?";
}

}  // namespace basis::model
