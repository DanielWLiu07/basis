#include "feed/binance_parser.h"

#include "feed/decimal.h"
#include "model/book_delta.h"

namespace basis::feed {

namespace {

using model::Action;
using model::BookDelta;
using model::Side;
using model::Venue;

bool push_level(ParseResult& out, std::string_view market, Side side,
                std::string_view price, std::string_view qty,
                std::int64_t recv_ns) {
  int cents = 0;
  std::int64_t size = 0;
  if (!to_exact_cents(price, &cents)) return false;
  if (!to_scaled_size(qty, &size)) return false;
  BookDelta d;
  d.venue = Venue::Binance;
  d.market = market;
  d.action = Action::Set;  // absolute level size; 0 removes
  d.side = side;
  d.price_cents = cents;
  d.size = size;
  d.ts_ns = recv_ns;
  out.deltas.push_back(d);
  return true;
}

}  // namespace

ParseResult BinanceParser::parse(std::string_view raw, std::int64_t recv_ns,
                                 std::pmr::memory_resource* mr) {
  ParseResult out(mr);
  simdjson::dom::element root;
  const auto err = parser_.parse(simdjson::padded_string(raw)).get(root);
  if (err != simdjson::SUCCESS) {
    out.status = ParseStatus::Malformed;
    return out;
  }

  // Combined-stream envelope. A bare (single-stream) payload has no
  // "stream" key; both shapes are accepted.
  simdjson::dom::element data = root;
  std::string_view stream;
  if (root["stream"].get_string().get(stream) == simdjson::SUCCESS) {
    if (root["data"].get(data) != simdjson::SUCCESS) {
      out.status = ParseStatus::Malformed;
      return out;
    }
  }

  std::string_view symbol;
  if (data["s"].get_string().get(symbol) != simdjson::SUCCESS) {
    // Subscription acks and other control frames carry no symbol.
    out.status = ParseStatus::Ignored;
    return out;
  }

  std::string_view event_type;
  const bool is_depth =
      data["e"].get_string().get(event_type) == simdjson::SUCCESS &&
      event_type == "depthUpdate";

  if (is_depth) {
    const auto side_of = [&](const char* key, Side side) -> bool {
      simdjson::dom::array levels;
      if (data[key].get_array().get(levels) != simdjson::SUCCESS) return true;
      for (auto level : levels) {
        simdjson::dom::array pair;
        if (level.get_array().get(pair) != simdjson::SUCCESS) return false;
        std::string_view price, qty;
        auto it = pair.begin();
        if (it == pair.end() || (*it).get_string().get(price) != simdjson::SUCCESS) {
          return false;
        }
        ++it;
        if (it == pair.end() || (*it).get_string().get(qty) != simdjson::SUCCESS) {
          return false;
        }
        if (!push_level(out, symbol, side, price, qty, recv_ns)) return false;
      }
      return true;
    };
    if (!side_of("b", Side::Bid) || !side_of("a", Side::Ask)) {
      out.deltas.clear();
      out.status = ParseStatus::Malformed;
      return out;
    }
    out.status = out.deltas.empty() ? ParseStatus::Ignored : ParseStatus::Ok;
    return out;
  }

  // bookTicker: b/B best bid and size, a/A best ask and size.
  std::string_view bid_px, bid_qty, ask_px, ask_qty;
  const bool has_book =
      data["b"].get_string().get(bid_px) == simdjson::SUCCESS &&
      data["B"].get_string().get(bid_qty) == simdjson::SUCCESS &&
      data["a"].get_string().get(ask_px) == simdjson::SUCCESS &&
      data["A"].get_string().get(ask_qty) == simdjson::SUCCESS;
  if (!has_book) {
    out.status = ParseStatus::Ignored;  // trade, kline, control frame
    return out;
  }
  // bookTicker REPLACES the touch; it is not a diff against the levels
  // already in the book. Without this Clear the old best bid and ask stay
  // resting, so best_bid() decays into the running maximum of every bid
  // ever quoted and best_ask() into the running minimum. Measured on a
  // 40-minute BTCUSDT capture, the book reached 204 phantom levels and a
  // spread of MINUS $54.87 by message 20,000, and the mid it produced
  // moved over a $25 band where the real one moved $55. Nothing caught it
  // because the only consumer of this stream measured throughput, where a
  // wrong book costs nothing.
  BookDelta reset;
  reset.venue = Venue::Binance;
  reset.market = symbol;
  reset.action = Action::Clear;
  reset.ts_ns = recv_ns;
  out.deltas.push_back(reset);
  if (!push_level(out, symbol, Side::Bid, bid_px, bid_qty, recv_ns) ||
      !push_level(out, symbol, Side::Ask, ask_px, ask_qty, recv_ns)) {
    out.deltas.clear();
    out.status = ParseStatus::Malformed;
    return out;
  }
  out.status = ParseStatus::Ok;
  return out;
}

}  // namespace basis::feed
