#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "model/types.h"

namespace basis::model {

// One executed trade on one venue.
//
// This engine carried price levels and nothing else, which made it a book
// engine rather than a market-data engine: a real feed carries what the
// book WOULD do and what it DID, and only the first half was here. Every
// venue in this repo publishes prints on the same socket the book arrives
// on.
//
// Why a separate type rather than a fourth Action on BookDelta. A trade is
// not a change to a level - it is an event that happened at a price, and
// the level it happened at may already be gone by the time it is
// published. Folding it into BookDelta would give every consumer of a book
// update a case it must ignore, and would let a trade silently mutate a
// book through the same apply() path that levels use. They are different
// facts and they get different types.
//
// `market` is a view into the producing parser's buffer, exactly as in
// BookDelta and for the same reason: Polymarket asset ids run ~77 digits
// and copying one per print would put an allocation on the hot path. Valid
// until the next parse() on the same parser.
struct Trade {
  Venue            venue       = Venue::Kalshi;
  std::string_view market;
  // Which side crossed the spread to make this happen, when the venue says
  // so. Coinbase reports the MAKER's side, so a "sell" print means a
  // resting sell was hit and the aggressor was a buyer; the parser inverts
  // it so this field always means the taker. Venues that do not publish it
  // leave it unknown rather than guessing, because guessing would corrupt
  // any order-flow measure built on it.
  Aggressor        aggressor   = Aggressor::Unknown;
  int              price_cents = 0;
  std::int64_t     size        = 0;  // scaled like BookDelta::size
  std::uint64_t    trade_id    = 0;  // venue's own id, 0 when absent
  std::uint64_t    seq         = 0;
  std::int64_t     ts_ns       = 0;  // ingest timestamp, ns since epoch
};

// An owning copy, for consumers that outlive the parse buffer. Same split
// and same reason as OwnedBookDelta: the live path queues across threads.
struct OwnedTrade {
  OwnedTrade() = default;
  explicit OwnedTrade(const Trade& t)
      : market(t.market),
        venue(t.venue),
        aggressor(t.aggressor),
        price_cents(t.price_cents),
        size(t.size),
        trade_id(t.trade_id),
        seq(t.seq),
        ts_ns(t.ts_ns) {}

  Trade view() const {
    return Trade{.venue = venue,
                 .market = market,
                 .aggressor = aggressor,
                 .price_cents = price_cents,
                 .size = size,
                 .trade_id = trade_id,
                 .seq = seq,
                 .ts_ns = ts_ns};
  }

  std::string   market;
  Venue         venue       = Venue::Kalshi;
  Aggressor     aggressor   = Aggressor::Unknown;
  int           price_cents = 0;
  std::int64_t  size        = 0;
  std::uint64_t trade_id    = 0;
  std::uint64_t seq         = 0;
  std::int64_t  ts_ns       = 0;
};

}  // namespace basis::model
