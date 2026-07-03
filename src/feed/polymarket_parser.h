#pragma once

#include <cstdint>
#include <memory_resource>
#include <string_view>

#include <simdjson.h>

#include "feed/parse_result.h"

namespace basis::feed {

// Parses Polymarket CLOB market-channel WebSocket messages into canonical
// deltas. Wire format notes (docs/api_integration.md):
//
//   book          snapshot: bids/asks arrays of {price, size} decimal
//                 strings, keyed by asset_id (one outcome token).
//   price_change  deltas: price_changes[] of {asset_id, price, size, side};
//                 size is the *absolute* level size (Action::Set), "0"
//                 removes the level; side is BUY/SELL.
//
// The top-level payload may be a single event or an array of events.
// Prices are decimal probability strings ("0.47") converted to canonical
// integer cents. Deltas are keyed by asset_id, the token the contract
// registry maps. Stream integrity is a book hash rather than a sequence
// number; verifying it needs the venue's book-hash algorithm and is wired
// with the live feed, not here.
class PolymarketParser {
 public:
  // `mr` backs the result's deltas vector; see ParseResult for lifetime.
  ParseResult parse(std::string_view raw, std::int64_t recv_ns,
                    std::pmr::memory_resource* mr =
                        std::pmr::get_default_resource());

 private:
  simdjson::dom::parser parser_;
};

}  // namespace basis::feed
