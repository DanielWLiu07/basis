#pragma once

#include <cstdint>
#include <memory_resource>
#include <string_view>

#include <simdjson.h>

#include "feed/parse_result.h"

namespace basis::feed {

// Parses Coinbase Exchange market data into canonical deltas.
//
// This venue exists in the engine for one reason the others cannot serve:
// it quotes the SAME instrument as another venue, with no credentials
// required. Kalshi and Polymarket quote matching contracts but the Kalshi
// side needs an API key, and Binance alone cannot be compared with
// anything. Binance BTCUSDT against Coinbase BTC-USD is the first pair
// this engine can observe simultaneously, which is what turns the
// lead-lag estimator from a closed-loop self-test on synthetic data into
// a measurement of two real venues (docs/bench/cross_venue_lead.md).
//
// Wire format (wss://ws-feed.exchange.coinbase.com, `ticker` channel):
//
//   {"type":"ticker","product_id":"BTC-USD","best_bid":"63748.90",
//    "best_bid_size":"0.05","best_ask":"63748.91","best_ask_size":"0.01",
//    ...}
//
// The ticker channel publishes on every trade and every top-of-book
// change, and each message carries the absolute best bid and ask rather
// than a diff, so both sides are Action::Set and no sequencing is needed.
// Other frame types on the same socket (subscriptions, heartbeat, and the
// full-channel messages) carry no top of book and are Ignored.
//
// The two representation decisions match the Binance parser exactly, and
// deliberately so: a price is canonical integer cents or the message is
// rejected as malformed rather than truncated, and a size is scaled by
// 1e8. Sharing the conversion (feed/decimal.h) is what makes a mid from
// one venue directly comparable with a mid from the other; a parser that
// rounded differently would put a fake basis between the venues.
class CoinbaseParser {
 public:
  // `mr` backs the result's deltas vector; see ParseResult for lifetime.
  ParseResult parse(std::string_view raw, std::int64_t recv_ns,
                    std::pmr::memory_resource* mr =
                        std::pmr::get_default_resource());

 private:
  simdjson::dom::parser parser_;
};

}  // namespace basis::feed
