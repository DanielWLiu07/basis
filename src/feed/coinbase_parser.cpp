#include "feed/coinbase_parser.h"

#include "feed/decimal.h"
#include "model/book_delta.h"

namespace basis::feed {

namespace {

using model::Action;
using model::BookDelta;
using model::Side;
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

  if (type != "ticker") {
    out.status = ParseStatus::Ignored;  // heartbeat, match, other channels
    return out;
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
    out.status = ParseStatus::Ignored;
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
