#include "feed/polymarket_parser.h"

#include <optional>

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
  if (i == s.size()) return value;  // no fractional part
  ++i;                              // skip '.'
  std::int64_t frac_scale = scale;
  for (; i < s.size(); ++i) {
    if (s[i] < '0' || s[i] > '9') return std::nullopt;
    const int digit = s[i] - '0';
    if (frac_scale >= 10) {
      frac_scale /= 10;
      value += digit * frac_scale;
    } else {
      // First digit past the kept precision decides rounding; the rest
      // cannot change it for the half-up rule we want here.
      if (digit >= 5) ++value;
      break;
    }
  }
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
