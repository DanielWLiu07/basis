#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

#include <simdjson.h>

#include "feed/parse_result.h"

namespace basis::feed {

// Parses Kalshi trade-api v2 WebSocket messages into canonical deltas.
// Wire format notes (docs/api_integration.md):
//
//   orderbook_snapshot  msg.yes / msg.no are [price_cents, size] arrays of
//                       resting *bids* on each contract side. A NO bid at p
//                       is a YES ask at 100 - p, so both fold into one
//                       YES-frame book: yes -> bids, no -> asks at 100 - p.
//   orderbook_delta     msg.delta is a signed size *change* (Action::Add) at
//                       msg.price on msg.side ("yes"/"no"), same folding.
//
// Messages carry a per-subscription sequence number. A gap means missed
// deltas: the parser prepends a Clear (the book must not go stale silently)
// and sets ParseResult::gap so the live feed can re-snapshot.
class KalshiParser {
 public:
  ParseResult parse(std::string_view raw, std::int64_t recv_ns);

 private:
  simdjson::dom::parser parser_;
  std::unordered_map<std::string, std::uint64_t> last_seq_;  // per market
};

}  // namespace basis::feed
