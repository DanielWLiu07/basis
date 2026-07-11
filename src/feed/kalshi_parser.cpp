#include "feed/kalshi_parser.h"

namespace basis::feed {

namespace {

using simdjson::NO_SUCH_FIELD;
using simdjson::SUCCESS;
using simdjson::dom::array;
using simdjson::dom::element;

// Kalshi wire prices are integer cents strictly inside the contract range.
// Validating on the raw int64, before any narrowing or folding, is what
// keeps 100 - price from fabricating a level (or overflowing an int).
bool valid_wire_price(std::int64_t price) { return price >= 1 && price <= 99; }

// Appends Set deltas for one side of a snapshot. Kalshi levels are
// [price_cents, size] pairs of resting bids on that contract side; NO bids
// land in the YES book as asks at 100 - price.
bool append_snapshot_side(const element& msg, const char* key,
                          model::Side side, bool fold_price,
                          const model::BookDelta& base,
                          std::pmr::vector<model::BookDelta>& out) {
  const auto field = msg[key];
  if (field.error() == NO_SUCH_FIELD) {
    return true;  // side absent entirely is a legal empty book
  }
  array levels;
  if (field.get_array().get(levels) != SUCCESS) {
    return false;  // present but not an array: corrupt, not empty
  }
  for (const element level : levels) {
    array pair;
    std::int64_t price = 0;
    std::int64_t size = 0;
    if (level.get_array().get(pair) != SUCCESS ||
        pair.at(0).get_int64().get(price) != SUCCESS ||
        pair.at(1).get_int64().get(size) != SUCCESS ||
        !valid_wire_price(price) || size < 0) {
      // A resting snapshot quantity is never negative; rejecting it keeps
      // the wire-validation story covering size, not just price.
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

ParseResult KalshiParser::parse(std::string_view raw, std::int64_t recv_ns,
                                std::pmr::memory_resource* mr) {
  ParseResult result(mr);

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
  std::uint64_t sid = 0;
  const bool has_seq = doc["seq"].get_uint64().get(seq) == SUCCESS &&
                       doc["sid"].get_uint64().get(sid) == SUCCESS;
  if (has_seq) result.sid = sid;

  const model::BookDelta base{.venue = model::Venue::Kalshi,
                              .market = ticker,
                              .seq = seq,
                              .ts_ns = recv_ns};

  if (is_snapshot) {
    // A snapshot is authoritative: clear first so old levels cannot linger,
    // and reset the subscription's sequence tracking to its number.
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
    if (has_seq) last_seq_[sid] = seq;
    snapshotted_.emplace(base.market);  // ledger outlives the view: copy
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
      (side != "yes" && side != "no") || !valid_wire_price(price)) {
    result.status = ParseStatus::Malformed;
    return result;
  }

  if (has_seq) {
    const auto it = last_seq_.find(sid);
    if (it != last_seq_.end() && seq != it->second + 1) {
      result.gap = true;  // missed at least one message on this subscription
    }
    last_seq_[sid] = seq;
  }
  if (snapshotted_.emplace(base.market).second) {
    // First sight of this market is a delta: there is no book to apply it
    // to. Flag once; the live feed answers a gap with a re-snapshot.
    result.gap = true;
  }
  if (result.gap) {
    model::BookDelta clear = base;
    clear.action = model::Action::Clear;
    result.deltas.push_back(std::move(clear));
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
