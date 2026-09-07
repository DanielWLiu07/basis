#include "feed/coinbase_parser.h"

#include "feed/decimal.h"
#include "model/book_delta.h"
#include "model/trade.h"

namespace basis::feed {

namespace {

using model::Action;
using model::BookDelta;
using model::Aggressor;
using model::Side;
using model::Trade;
using model::Venue;

// A level this engine cannot represent is skipped and counted, not treated
// as a broken message: see ParseResult::levels_unrepresentable for why the
// distinction is load-bearing here. Returns false only for a genuinely
// malformed level.
bool push_level(ParseResult& out, std::string_view market, Side side,
                std::string_view price, std::string_view qty,
                std::int64_t recv_ns) {
  int cents = 0;
  std::int64_t size = 0;
  switch (parse_cents(price, &cents)) {
    case PriceParse::Ok: break;
    case PriceParse::OutOfRange:
      ++out.levels_unrepresentable;
      return true;
    case PriceParse::OffGrid:
    case PriceParse::NotANumber:
      return false;
  }
  if (!to_scaled_size(qty, &size)) return false;
  BookDelta d;
  d.venue = Venue::Coinbase;
  d.market = market;
  d.action = Action::Set;  // absolute top of book; 0 removes
  d.side = side;
  d.price_cents = cents;
  d.size = size;
  d.ts_ns = recv_ns;
  out.deltas.push_back(d);
  return true;
}

// The print carried by a ticker or match frame.
//
// Coinbase reports the MAKER's side: a frame with "side":"sell" means a
// resting sell order was hit, so the aggressor was a buyer. The field is
// inverted here rather than at every call site, because getting it
// backwards flips the sign of any order-flow measure built on it and the
// mistake is invisible in the data - both values are plausible.
//
// A frame without a price or size is not a trade. Coinbase publishes a
// ticker for a product that has never traded, and its book fields are
// absent too; that is an absence of data, not a broken message.
bool push_trade(ParseResult& out, std::string_view market,
                std::string_view price, std::string_view qty,
                std::string_view maker_side, std::uint64_t trade_id,
                std::int64_t recv_ns) {
  int cents = 0;
  std::int64_t size = 0;
  switch (parse_cents(price, &cents)) {
    case PriceParse::Ok: break;
    case PriceParse::OutOfRange:
      ++out.levels_unrepresentable;
      return true;
    case PriceParse::OffGrid:
    case PriceParse::NotANumber:
      return false;
  }
  if (!to_scaled_size(qty, &size)) return false;
  Trade t;
  t.venue = Venue::Coinbase;
  t.market = market;
  t.price_cents = cents;
  t.size = size;
  t.trade_id = trade_id;
  t.ts_ns = recv_ns;
  if (maker_side == "sell")      t.aggressor = Aggressor::Buy;
  else if (maker_side == "buy")  t.aggressor = Aggressor::Sell;
  else                           t.aggressor = Aggressor::Unknown;
  out.trades.push_back(t);
  return true;
}

}  // namespace

ParseResult CoinbaseParser::parse(std::string_view raw, std::int64_t recv_ns,
                                  std::pmr::memory_resource* mr) {
  ParseResult out(mr);
  simdjson::dom::element root;
  if (parser_.parse(simdjson::padded_string(raw)).get(root) !=
      simdjson::SUCCESS) {
    out.status = ParseStatus::Malformed;
    return out;
  }

  std::string_view type;
  if (root["type"].get_string().get(type) != simdjson::SUCCESS) {
    out.status = ParseStatus::Ignored;  // subscriptions ack, heartbeat
    return out;
  }

  std::string_view product;
  if (root["product_id"].get_string().get(product) != simdjson::SUCCESS) {
    // Control frames carry no product; that is not a broken message.
    out.status = ParseStatus::Ignored;
    return out;
  }

  // level2_batch: a full book image, then diffs against it. The snapshot
  // leads with Action::Clear so a mid-session reconnect (which re-sends a
  // snapshot) rebuilds the book instead of layering a new image over stale
  // levels that the new one happens not to mention.
  if (type == "snapshot") {
    BookDelta reset;
    reset.venue = Venue::Coinbase;
    reset.market = product;
    reset.action = Action::Clear;
    reset.ts_ns = recv_ns;
    out.deltas.push_back(reset);
    const auto side_of = [&](const char* key, Side side) -> bool {
      simdjson::dom::array levels;
      if (root[key].get_array().get(levels) != simdjson::SUCCESS) return true;
      for (auto level : levels) {
        simdjson::dom::array pair;
        if (level.get_array().get(pair) != simdjson::SUCCESS) return false;
        auto it = pair.begin();
        std::string_view px, sz;
        if (it == pair.end() ||
            (*it).get_string().get(px) != simdjson::SUCCESS) return false;
        ++it;
        if (it == pair.end() ||
            (*it).get_string().get(sz) != simdjson::SUCCESS) return false;
        if (!push_level(out, product, side, px, sz, recv_ns)) return false;
      }
      return true;
    };
    if (!side_of("bids", Side::Bid) || !side_of("asks", Side::Ask)) {
      out.deltas.clear();
      out.status = ParseStatus::Malformed;
      return out;
    }
    out.status = ParseStatus::Ok;
    return out;
  }

  if (type == "l2update") {
    simdjson::dom::array changes;
    if (root["changes"].get_array().get(changes) != simdjson::SUCCESS) {
      out.status = ParseStatus::Malformed;
      return out;
    }
    for (auto change : changes) {
      simdjson::dom::array triple;
      if (change.get_array().get(triple) != simdjson::SUCCESS) {
        out.deltas.clear();
        out.status = ParseStatus::Malformed;
        return out;
      }
      auto it = triple.begin();
      std::string_view side_txt, px, sz;
      const bool shaped =
          it != triple.end() &&
          (*it).get_string().get(side_txt) == simdjson::SUCCESS &&
          ++it != triple.end() &&
          (*it).get_string().get(px) == simdjson::SUCCESS &&
          ++it != triple.end() &&
          (*it).get_string().get(sz) == simdjson::SUCCESS;
      if (!shaped || (side_txt != "buy" && side_txt != "sell")) {
        out.deltas.clear();
        out.status = ParseStatus::Malformed;
        return out;
      }
      const Side side = side_txt == "buy" ? Side::Bid : Side::Ask;
      if (!push_level(out, product, side, px, sz, recv_ns)) {
        out.deltas.clear();
        out.status = ParseStatus::Malformed;
        return out;
      }
    }
    out.status = out.deltas.empty() ? ParseStatus::Ignored : ParseStatus::Ok;
    return out;
  }

  // The match channel is trades and nothing else: no book fields, so it
  // produces a print and no deltas.
  if (type == "match" || type == "last_match") {
    std::string_view px, sz, maker_side;
    if (root["price"].get_string().get(px) != simdjson::SUCCESS ||
        root["size"].get_string().get(sz) != simdjson::SUCCESS) {
      out.status = ParseStatus::Ignored;
      return out;
    }
    if (root["side"].get_string().get(maker_side) != simdjson::SUCCESS) {
      maker_side = {};
    }
    std::uint64_t tid = 0;
    if (root["trade_id"].get_uint64().get(tid) != simdjson::SUCCESS) tid = 0;
    if (!push_trade(out, product, px, sz, maker_side, tid, recv_ns)) {
      out.trades.clear();
      out.status = ParseStatus::Malformed;
      return out;
    }
    out.status = ParseStatus::Ok;
    return out;
  }

  if (type != "ticker") {
    out.status = ParseStatus::Ignored;  // heartbeat, other channels
    return out;
  }

  // A ticker is BOTH a print and a touch update, which is why the two
  // live in separate vectors on one result. The print is emitted first
  // and independently: a ticker whose book fields are missing still
  // carries a trade, and dropping it because the touch was absent was
  // throwing away half of what the frame says.
  {
    std::string_view px, last_size, maker_side;
    if (root["price"].get_string().get(px) == simdjson::SUCCESS &&
        root["last_size"].get_string().get(last_size) == simdjson::SUCCESS) {
      if (root["side"].get_string().get(maker_side) != simdjson::SUCCESS) {
        maker_side = {};
      }
      std::uint64_t tid = 0;
      if (root["trade_id"].get_uint64().get(tid) != simdjson::SUCCESS) tid = 0;
      if (!push_trade(out, product, px, last_size, maker_side, tid, recv_ns)) {
        out.trades.clear();
        out.status = ParseStatus::Malformed;
        return out;
      }
    }
  }

  // A ticker for a product that has never traded omits the book fields
  // entirely; that is an absence of data, not a broken message.
  std::string_view bid_px, bid_sz, ask_px, ask_sz;
  const bool has_book =
      root["best_bid"].get_string().get(bid_px) == simdjson::SUCCESS &&
      root["best_bid_size"].get_string().get(bid_sz) == simdjson::SUCCESS &&
      root["best_ask"].get_string().get(ask_px) == simdjson::SUCCESS &&
      root["best_ask_size"].get_string().get(ask_sz) == simdjson::SUCCESS;
  if (!has_book) {
    // No touch, but the print above may still have landed.
    out.status = out.trades.empty() ? ParseStatus::Ignored : ParseStatus::Ok;
    return out;
  }

  if (!push_level(out, product, Side::Bid, bid_px, bid_sz, recv_ns) ||
      !push_level(out, product, Side::Ask, ask_px, ask_sz, recv_ns)) {
    out.deltas.clear();
    out.status = ParseStatus::Malformed;
    return out;
  }
  out.status = ParseStatus::Ok;
  return out;
}

}  // namespace basis::feed
