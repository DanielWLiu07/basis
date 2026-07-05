#pragma once

#include <cstdint>
#include <string>
#include <string_view>

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
//
// `market` is a view into the producing parser's buffer, not owned storage:
// Polymarket asset ids run ~77 digits, and copying one per delta would put
// a heap allocation on every message. The view is valid until the next
// parse() on the same parser, so deltas must be consumed synchronously;
// anything that retains a delta (the Kalshi parser's snapshot ledger, the
// normalizer's event map) copies what it keeps.
struct BookDelta {
  Venue            venue       = Venue::Kalshi;
  std::string_view market;          // venue-native market id (see above)
  Action           action      = Action::Set;
  Side             side        = Side::Bid;
  int              price_cents = 0; // parsers enforce 0..100 (Kalshi wire: 1..99)
  std::int64_t     size        = 0; // meaning depends on action
  std::uint64_t    seq         = 0; // venue sequence number (gap detection)
  std::int64_t     ts_ns       = 0; // ingest timestamp, ns since epoch
};

// An owning copy of a BookDelta, for consumers that outlive the parse
// buffer: the live path queues deltas from the IO threads to the
// analytics thread, and a queued view would dangle by the time it is
// popped. Fields are stored flat (no internal aliasing), so moves and
// copies are safe by construction; view() re-materializes a BookDelta
// whose market aliases this object, valid for this object's lifetime.
struct OwnedBookDelta {
  OwnedBookDelta() = default;
  explicit OwnedBookDelta(const BookDelta& d)
      : market(d.market),
        venue(d.venue),
        action(d.action),
        side(d.side),
        price_cents(d.price_cents),
        size(d.size),
        seq(d.seq),
        ts_ns(d.ts_ns) {}

  BookDelta view() const {
    return BookDelta{.venue = venue,
                     .market = market,
                     .action = action,
                     .side = side,
                     .price_cents = price_cents,
                     .size = size,
                     .seq = seq,
                     .ts_ns = ts_ns};
  }

  std::string   market;
  Venue         venue       = Venue::Kalshi;
  Action        action      = Action::Set;
  Side          side        = Side::Bid;
  int           price_cents = 0;
  std::int64_t  size        = 0;
  std::uint64_t seq         = 0;
  std::int64_t  ts_ns       = 0;
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
