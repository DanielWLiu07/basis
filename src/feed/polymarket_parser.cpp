#include "feed/polymarket_parser.h"

#include <optional>
#include <string>

#include "core/sha1.h"

namespace basis::feed {

namespace {

using simdjson::NO_SUCH_FIELD;
using simdjson::SUCCESS;
using simdjson::dom::array;
using simdjson::dom::element;

// Parses a non-negative decimal string ("0.475") into a value scaled by
// `scale` (100 for probability -> cents, 1 for sizes), rounding half up on
// the digit after the last kept one. Hand-rolled to stay locale-independent
// and allocation-free; std::from_chars for floating point is still missing
// from Apple's libc++.
std::optional<std::int64_t> parse_scaled_decimal(std::string_view s,
                                                 std::int64_t scale) {
  if (s.empty()) return std::nullopt;
  std::int64_t value = 0;  // integer part
  std::size_t i = 0;
  int digits = 0;
  for (; i < s.size() && s[i] != '.'; ++i) {
    if (s[i] < '0' || s[i] > '9') return std::nullopt;
    // No real probability or size needs 16 integer digits; past that the
    // arithmetic below would overflow, so it is malformed, not big.
    if (++digits > 15) return std::nullopt;
    value = value * 10 + (s[i] - '0');
  }
  value *= scale;
  if (i == s.size()) {
    // A field with no digit at all ("." or "") is malformed, not zero;
    // accepting it would fabricate a level at price 0.
    return digits > 0 ? std::optional<std::int64_t>(value) : std::nullopt;
  }
  ++i;  // skip '.'
  std::int64_t frac_scale = scale;
  int frac_digits = 0;
  bool rounded = false;
  for (; i < s.size(); ++i) {
    // Every remaining byte must be a digit: trailing garbage after the
    // last significant place ("0.505junk") is malformed, not truncatable.
    if (s[i] < '0' || s[i] > '9') return std::nullopt;
    ++frac_digits;
    const int digit = s[i] - '0';
    if (frac_scale >= 10) {
      frac_scale /= 10;
      value += digit * frac_scale;
    } else if (!rounded) {
      // First digit past the kept precision decides half-up rounding once;
      // further digits are validated above but cannot change the result.
      if (digit >= 5) ++value;
      rounded = true;
    }
  }
  if (digits == 0 && frac_digits == 0) return std::nullopt;  // bare "."
  return value;
}

// Polymarket prices are probabilities, so the only valid cent range is
// 0..100. Rejecting here (on the wide int64, before narrowing) is what
// keeps a corrupt price from wrapping into a plausible-looking level.
std::optional<int> parse_price_cents(std::string_view s) {
  const auto v = parse_scaled_decimal(s, 100);
  if (!v || *v < 0 || *v > 100) return std::nullopt;
  return static_cast<int>(*v);
}

// Level sizes are decimal share counts and may be fractional. The canonical
// schema stores whole contracts; a real-but-tiny level must not round to
// zero, because zero means "remove".
std::optional<std::int64_t> parse_size(std::string_view s) {
  const auto v = parse_scaled_decimal(s, 1);
  if (!v) return std::nullopt;
  if (*v == 0 && s.find_first_of("123456789") != std::string_view::npos) {
    return 1;
  }
  return v;
}

bool append_book_side(const element& event, const char* key, model::Side side,
                      const model::BookDelta& base,
                      std::pmr::vector<model::BookDelta>& out) {
  const auto field = event[key];
  if (field.error() == NO_SUCH_FIELD) {
    return true;  // absent side is a legal empty book
  }
  array levels;
  if (field.get_array().get(levels) != SUCCESS) {
    return false;  // present but not an array: corrupt, not empty
  }
  for (const element level : levels) {
    std::string_view price_s;
    std::string_view size_s;
    if (level["price"].get_string().get(price_s) != SUCCESS ||
        level["size"].get_string().get(size_s) != SUCCESS) {
      return false;
    }
    const auto price = parse_price_cents(price_s);
    const auto size = parse_size(size_s);
    if (!price || !size) return false;
    model::BookDelta d = base;
    d.side = side;
    d.price_cents = *price;
    d.size = *size;
    out.push_back(std::move(d));
  }
  return true;
}

// Rebuilds the venue's canonical order book summary and checks its SHA-1
// against the hash the message carries. The payload layout matches the
// server's struct serialization, established against Polymarket's own
// client library and confirmed byte-for-byte on recorded live sessions:
//
//   {"market":M,"asset_id":A,"timestamp":T,"hash":"","bids":[...],
//    "asks":[...],"min_order_size":"5","tick_size":TS,"neg_risk":NR,
//    "last_trade_price":LTP}
//
// Two fields need care. min_order_size is not on the wire; "5" is the
// platform default and matched every observed market, and a market where
// it differs shows up as Mismatched, never as silently verified.
// neg_risk is not on the wire either, so both values are tried; that
// weakens the check by one bit, not its ability to catch a corrupted
// book. Values are spliced back verbatim (ids and decimal strings need
// no JSON escaping), so a level the parser rejected can never be
// laundered into a verified book.
enum class HashCheck : std::uint8_t {
  NotApplicable,
  Verified,
  Mismatched,
  Unverifiable,
};

HashCheck verify_book_hash(const element& event) {
  std::string_view market;
  std::string_view asset_id;
  std::string_view timestamp;
  std::string_view wire_hash;
  if (event["hash"].get_string().get(wire_hash) != SUCCESS) {
    return HashCheck::NotApplicable;
  }
  if (event["market"].get_string().get(market) != SUCCESS ||
      event["asset_id"].get_string().get(asset_id) != SUCCESS ||
      event["timestamp"].get_string().get(timestamp) != SUCCESS) {
    return HashCheck::Unverifiable;
  }
  // The venue hashes over tick_size and last_trade_price, but only its
  // subscribe-time snapshots carry them; periodic refreshes omit them
  // and are honestly unverifiable from the wire alone.
  std::string_view tick_size;
  std::string_view last_trade_price;
  if (event["tick_size"].get_string().get(tick_size) != SUCCESS ||
      event["last_trade_price"].get_string().get(last_trade_price) !=
          SUCCESS) {
    return HashCheck::Unverifiable;
  }

  std::string sides;
  for (const char* key : {"bids", "asks"}) {
    sides += (key[0] == 'b') ? R"("bids":[)" : R"(,"asks":[)";
    array levels;
    if (event[key].get_array().get(levels) != SUCCESS) {
      return HashCheck::Unverifiable;
    }
    bool first = true;
    for (const element level : levels) {
      std::string_view price;
      std::string_view size;
      if (level["price"].get_string().get(price) != SUCCESS ||
          level["size"].get_string().get(size) != SUCCESS) {
        return HashCheck::Unverifiable;
      }
      if (!first) sides += ',';
      first = false;
      sides += R"({"price":")";
      sides += price;
      sides += R"(","size":")";
      sides += size;
      sides += R"("})";
    }
    sides += ']';
  }

  for (const char* neg_risk : {"true", "false"}) {
    std::string payload = R"({"market":")";
    payload += market;
    payload += R"(","asset_id":")";
    payload += asset_id;
    payload += R"(","timestamp":")";
    payload += timestamp;
    payload += R"(","hash":"",)";
    payload += sides;
    payload += R"(,"min_order_size":"5","tick_size":")";
    payload += tick_size;
    payload += R"(","neg_risk":)";
    payload += neg_risk;
    payload += R"(,"last_trade_price":")";
    payload += last_trade_price;
    payload += R"("})";
    if (core::sha1_hex(payload) == wire_hash) {
      return HashCheck::Verified;
    }
  }
  return HashCheck::Mismatched;
}

// Handles one event object. Returns false on structural damage.
bool handle_event(const element& event, std::int64_t recv_ns,
                  ParseResult& result) {
  std::string_view event_type;
  if (event["event_type"].get_string().get(event_type) != SUCCESS) {
    return true;  // not an event we know; Ignored
  }

  if (event_type == "book") {
    std::string_view asset_id;
    if (event["asset_id"].get_string().get(asset_id) != SUCCESS) return false;
    const model::BookDelta base{.venue = model::Venue::Polymarket,
                                .market = asset_id,
                                .ts_ns = recv_ns};
    // Snapshot: clear, then set every level.
    model::BookDelta clear = base;
    clear.action = model::Action::Clear;
    result.deltas.push_back(std::move(clear));
    if (!append_book_side(event, "bids", model::Side::Bid, base,
                          result.deltas) ||
        !append_book_side(event, "asks", model::Side::Ask, base,
                          result.deltas)) {
      return false;
    }
    switch (verify_book_hash(event)) {
      case HashCheck::Verified:     ++result.hashes_verified; break;
      case HashCheck::Mismatched:   ++result.hashes_mismatched; break;
      case HashCheck::Unverifiable: ++result.hashes_unverifiable; break;
      case HashCheck::NotApplicable: break;
    }
    return true;
  }

  if (event_type == "price_change") {
    array changes;
    if (event["price_changes"].get_array().get(changes) != SUCCESS) {
      return false;
    }
    for (const element change : changes) {
      std::string_view asset_id;
      std::string_view price_s;
      std::string_view size_s;
      std::string_view side_s;
      if (change["asset_id"].get_string().get(asset_id) != SUCCESS ||
          change["price"].get_string().get(price_s) != SUCCESS ||
          change["size"].get_string().get(size_s) != SUCCESS ||
          change["side"].get_string().get(side_s) != SUCCESS ||
          (side_s != "BUY" && side_s != "SELL")) {
        return false;
      }
      const auto price = parse_price_cents(price_s);
      const auto size = parse_size(size_s);
      if (!price || !size) return false;
      result.deltas.push_back(
          {.venue = model::Venue::Polymarket,
           .market = asset_id,
           .action = model::Action::Set,
           .side = side_s == "BUY" ? model::Side::Bid : model::Side::Ask,
           .price_cents = *price,
           .size = *size,
           .ts_ns = recv_ns});
    }
    return true;
  }

  return true;  // other event types (tick_size_change, ...) are Ignored
}

}  // namespace

ParseResult PolymarketParser::parse(std::string_view raw,
                                    std::int64_t recv_ns,
                                    std::pmr::memory_resource* mr) {
  ParseResult result(mr);

  element doc;
  if (parser_.parse(raw.data(), raw.size()).get(doc) != SUCCESS) {
    result.status = ParseStatus::Malformed;
    return result;
  }

  bool ok = true;
  array events;
  if (doc.get_array().get(events) == SUCCESS) {
    for (const element event : events) {
      if (!handle_event(event, recv_ns, result)) {
        ok = false;
        break;
      }
    }
  } else {
    ok = handle_event(doc, recv_ns, result);
  }

  if (!ok) {
    result.deltas.clear();
    result.status = ParseStatus::Malformed;
  } else if (!result.deltas.empty()) {
    result.status = ParseStatus::Ok;
  }
  return result;
}

}  // namespace basis::feed
