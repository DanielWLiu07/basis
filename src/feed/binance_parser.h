#pragma once

#include <cstdint>
#include <memory_resource>
#include <string_view>

#include <simdjson.h>

#include "feed/parse_result.h"

namespace basis::feed {

// Parses Binance combined-stream market data into canonical deltas. The
// venue exists in this engine for a reason the prediction markets cannot
// serve: it produces load. A busy prediction-market capture runs about 19
// messages/sec, which measures nothing; the same pipeline on Binance
// sustains hundreds/sec from a live socket (docs/bench/ingest.md).
//
// Wire format (combined stream, {"stream":..., "data":...}):
//
//   <sym>@bookTicker   best bid and ask with sizes, pushed on every change
//                      of either. Absolute values, so both sides are
//                      Action::Set.
//   <sym>@depth[@100ms] depthUpdate: b/a arrays of [price, qty] strings.
//                      qty is the absolute size at that level and "0"
//                      removes it, which is Action::Set as well.
//
// Two representation decisions, both forced by the engine's price model
// and both explicit rather than silent:
//
//   Prices are canonical integer cents, because prediction-market
//   contracts settle in cents and the whole engine is built on that. A
//   Binance price off the cent grid therefore cannot be represented
//   faithfully, so the parser REJECTS the message as malformed instead of
//   truncating it. The live adapter subscribes only to symbols whose
//   PRICE_FILTER tick is 0.01 (94 of them at the time of writing), and a
//   committed capture of those symbols contains 73,302 prices with zero
//   off-grid, so the restriction is checked rather than assumed.
//
//   Sizes are scaled by 1e8, the venue's own base-unit convention, into
//   the int64 size field. A fractional quantity that would round to zero
//   at that scale is a level removal in Binance's protocol anyway.
//
// There is no sequence-gap handling here. Binance's depthUpdate carries
// first/last update ids (U/u) for reconciliation against a REST snapshot,
// which the offline path does not take; the ingest benchmark measures the
// parse and book-apply path, and a live adapter that wants a coherent
// book must add that reconciliation.
class BinanceParser {
 public:
  // `mr` backs the result's deltas vector; see ParseResult for lifetime.
  ParseResult parse(std::string_view raw, std::int64_t recv_ns,
                    std::pmr::memory_resource* mr =
                        std::pmr::get_default_resource());

 private:
  simdjson::dom::parser parser_;
};

}  // namespace basis::feed
