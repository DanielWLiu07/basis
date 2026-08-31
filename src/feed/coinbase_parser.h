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
// Wire formats (wss://ws-feed.exchange.coinbase.com). Three message types
// are handled, and which one you get depends on the channel subscribed:
//
//   level2_batch -> "snapshot" then "l2update"   <- what the live feed uses
//
//     {"type":"snapshot","product_id":"BTC-USD",
//      "bids":[["63748.90","0.05"],...],"asks":[["63748.91","0.01"],...]}
//     {"type":"l2update","product_id":"BTC-USD",
//      "changes":[["buy","63748.90","0.03"],...]}
//
//   A full book image followed by diffs against it. The snapshot leads
//   with Action::Clear, so the re-snapshot Coinbase sends on reconnect
//   rebuilds the book rather than layering a new image over stale levels
//   the new one happens not to mention. Changes carry the absolute size
//   at a level, so they are Action::Set and a zero removes it.
//
//   ticker -> "ticker"                            <- parsed, not subscribed
//
//     {"type":"ticker","product_id":"BTC-USD","best_bid":"63748.90",
//      "best_bid_size":"0.05","best_ask":"63748.91",...}
//
//   Top of book only, absolute rather than diffed. Cheaper and shallower;
//   the depth the crossed-book economics need is not in it.
//
// This header used to document only the ticker form and describe it as
// the wire format, which was misleading in a way worth naming: it said
// "no sequencing is needed", and that is a claim about ticker that does
// not transfer to the channel actually in use. level2_batch IS a
// snapshot-plus-diff protocol. It needs no sequence NUMBERS because
// Coinbase does not publish them on this channel and guarantees delivery
// order, which is a different and weaker guarantee than the update-id
// continuity Binance provides and feed/book_sequencer.h checks.
//
// Everything else on the socket (subscriptions, heartbeat, full-channel
// frames) carries no book and is Ignored.
//
// Trades are NOT parsed. The ticker channel would carry them and the live
// feed does not subscribe to it, so no committed capture contains any.
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
