#include "feed/kalshi_parser.h"

#include "feed/decimal.h"

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

// One snapshot level, in either wire form. Kalshi's 2026 change moved
// levels from [51, 1200] integer pairs to ["0.5100", "1200.50"] decimal
// strings, so both are accepted: the committed captures under docs/bench/
// predate the change and must stay replayable byte for byte.
bool parse_level(const element& level, std::int64_t* price_cents,
                 std::int64_t* size) {
  array pair;
  if (level.get_array().get(pair) != SUCCESS) return false;
  std::string_view text;
  if (pair.at(0).get_string().get(text) == SUCCESS) {
    int cents = 0;
    // Exact or not at all: a sub-cent price (Kalshi now lists some markets
    // on a 0.0001 grid) has no representation in this book and must not be
    // rounded into a neighbouring one.
    if (!to_exact_cents(text, &cents)) return false;
    *price_cents = cents;
  } else if (pair.at(0).get_int64().get(*price_cents) != SUCCESS) {
    return false;
  }
  if (pair.at(1).get_string().get(text) == SUCCESS) {
    if (!to_rounded_contracts(text, size)) return false;
  } else if (pair.at(1).get_int64().get(*size) != SUCCESS) {
    return false;
  }
  return true;
}

// Appends Set deltas for one side of a snapshot. Kalshi levels are
// [price_cents, size] pairs of resting bids on that contract side; NO bids
// land in the YES book as asks at 100 - price. `key` is the current field
// name and `legacy_key` the pre-2026 one.
bool append_snapshot_side(const element& msg, const char* key,
                          const char* legacy_key, model::Side side,
                          bool fold_price, const model::BookDelta& base,
                          std::pmr::vector<model::BookDelta>& out,
                          bool* saw_side) {
  auto field = msg[key];
  if (field.error() == NO_SUCH_FIELD) field = msg[legacy_key];
  if (field.error() == NO_SUCH_FIELD) {
    // Absent under BOTH names. An empty Kalshi book still carries the key
    // with an empty array, so this is a schema the parser does not know,
    // not an empty market - see the caller, which refuses to call it Ok.
    return true;
  }
  *saw_side = true;
  array levels;
  if (field.get_array().get(levels) != SUCCESS) {
    return false;  // present but not an array: corrupt, not empty
  }
  for (const element level : levels) {
    std::int64_t price = 0;
    std::int64_t size = 0;
    if (!parse_level(level, &price, &size) || !valid_wire_price(price) ||
        size < 0) {
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
    bool saw_side = false;
    if (!append_snapshot_side(msg, "yes_dollars_fp", "yes", model::Side::Bid,
                              false, base, result.deltas, &saw_side) ||
        !append_snapshot_side(msg, "no_dollars_fp", "no", model::Side::Ask,
                              true, base, result.deltas, &saw_side) ||
        !saw_side) {
      // !saw_side is the schema-drift guard. Treating an unrecognised
      // snapshot as an empty book is what let a venue wire change sit
      // undetected behind healthy-looking counters: every snapshot
      // "parsed", every book was empty, and malformed stayed at zero.
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
  // Current wire: price_dollars/delta_fp as decimal strings. Pre-2026:
  // price/delta as integers. A delta size is signed - a resting level
  // shrinks as well as grows - which is why it does not go through the
  // unsigned size path the snapshot levels use.
  bool fields_ok = false;
  std::string_view text;
  if (msg["price_dollars"].get_string().get(text) == SUCCESS) {
    int cents = 0;
    fields_ok = to_exact_cents(text, &cents);
    price = cents;
    if (fields_ok) {
      fields_ok = msg["delta_fp"].get_string().get(text) == SUCCESS &&
                  to_rounded_contracts(text, &size_change);
    }
  } else {
    fields_ok = msg["price"].get_int64().get(price) == SUCCESS &&
                msg["delta"].get_int64().get(size_change) == SUCCESS;
  }
  if (!fields_ok || msg["side"].get_string().get(side) != SUCCESS ||
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
