#include "feed/kalshi_parser.h"

namespace basis::feed {

namespace {

using simdjson::SUCCESS;
using simdjson::dom::array;
using simdjson::dom::element;

// Appends Set deltas for one side of a snapshot. Kalshi levels are
// [price_cents, size] pairs of resting bids on that contract side; NO bids
// land in the YES book as asks at 100 - price.
bool append_snapshot_side(const element& msg, const char* key,
                          model::Side side, bool fold_price,
                          const model::BookDelta& base,
                          std::vector<model::BookDelta>& out) {
  array levels;
  if (msg[key].get_array().get(levels) != SUCCESS) {
    return true;  // side absent entirely is a legal empty book
  }
  for (const element level : levels) {
    array pair;
    std::int64_t price = 0;
    std::int64_t size = 0;
    if (level.get_array().get(pair) != SUCCESS ||
        pair.at(0).get_int64().get(price) != SUCCESS ||
        pair.at(1).get_int64().get(size) != SUCCESS) {
      return false;
    }
    model::BookDelta d = base;
    d.side = side;
    d.price_cents = fold_price ? 100 - static_cast<int>(price)
                               : static_cast<int>(price);
    d.size = size;
    out.push_back(std::move(d));
  }
  return true;
}

}  // namespace

ParseResult KalshiParser::parse(std::string_view raw, std::int64_t recv_ns) {
  ParseResult result;

  element doc;
  if (parser_.parse(raw.data(), raw.size()).get(doc) != SUCCESS) {
    result.status = ParseStatus::Malformed;
    return result;
  }

  std::string_view type;
  if (doc["type"].get_string().get(type) != SUCCESS) {
    return result;  // no type field: command replies etc, Ignored
  }
  const bool is_snapshot = type == "orderbook_snapshot";
  const bool is_delta = type == "orderbook_delta";
  if (!is_snapshot && !is_delta) {
    return result;  // subscribed/ok/error replies, Ignored
  }

  element msg;
  std::string_view ticker;
  if (doc["msg"].get(msg) != SUCCESS ||
      msg["market_ticker"].get_string().get(ticker) != SUCCESS) {
    result.status = ParseStatus::Malformed;
    return result;
  }
  std::uint64_t seq = 0;
  const bool has_seq = doc["seq"].get_uint64().get(seq) == SUCCESS;

  const model::BookDelta base{.venue = model::Venue::Kalshi,
                              .market = std::string(ticker),
                              .seq = seq,
                              .ts_ns = recv_ns};

  if (is_snapshot) {
    // A snapshot is authoritative: clear first so old levels cannot linger,
    // and reset sequence tracking to the snapshot's number.
    model::BookDelta clear = base;
    clear.action = model::Action::Clear;
    result.deltas.push_back(std::move(clear));
    if (!append_snapshot_side(msg, "yes", model::Side::Bid, false, base,
                              result.deltas) ||
        !append_snapshot_side(msg, "no", model::Side::Ask, true, base,
                              result.deltas)) {
      result.deltas.clear();
      result.status = ParseStatus::Malformed;
      return result;
    }
    if (has_seq) last_seq_[base.market] = seq;
    result.status = ParseStatus::Ok;
    return result;
  }

  // orderbook_delta
  std::int64_t price = 0;
  std::int64_t size_change = 0;
  std::string_view side;
  if (msg["price"].get_int64().get(price) != SUCCESS ||
      msg["delta"].get_int64().get(size_change) != SUCCESS ||
      msg["side"].get_string().get(side) != SUCCESS ||
      (side != "yes" && side != "no")) {
    result.status = ParseStatus::Malformed;
    return result;
  }

  if (has_seq) {
    const auto it = last_seq_.find(base.market);
    if (it != last_seq_.end() && seq != it->second + 1) {
      // Missed at least one delta: the local book is untrustworthy.
      result.gap = true;
      model::BookDelta clear = base;
      clear.action = model::Action::Clear;
      result.deltas.push_back(std::move(clear));
    }
    last_seq_[base.market] = seq;
  }

  model::BookDelta d = base;
  d.action = model::Action::Add;
  d.side = (side == "yes") ? model::Side::Bid : model::Side::Ask;
  d.price_cents = (side == "yes") ? static_cast<int>(price)
                                  : 100 - static_cast<int>(price);
  d.size = size_change;
  result.deltas.push_back(std::move(d));
  result.status = ParseStatus::Ok;
  return result;
}

}  // namespace basis::feed
