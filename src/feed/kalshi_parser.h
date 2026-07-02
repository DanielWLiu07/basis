#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

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
// Prices on the wire are integer cents in 1..99; anything else is counted
// as Malformed, never folded (the fold would fabricate a nonsense level).
//
// Gap detection: messages carry a sequence number that is contiguous per
// subscription (sid), not per market, so gaps are tracked by sid. A gap, or
// a delta for a market that never had a snapshot, makes the local book
// untrustworthy: the parser prepends a Clear (state must not go stale
// silently) and sets ParseResult::gap. Only the current message's market is
// cleared here; the live feed reacts to gap by re-snapshotting the whole
// subscription, which clears the rest.
class KalshiParser {
 public:
  ParseResult parse(std::string_view raw, std::int64_t recv_ns);

 private:
  simdjson::dom::parser parser_;
  std::unordered_map<std::uint64_t, std::uint64_t> last_seq_;  // per sid
  std::unordered_set<std::string> snapshotted_;  // markets with a snapshot
};

}  // namespace basis::feed
