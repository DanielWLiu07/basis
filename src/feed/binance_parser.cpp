#include "feed/binance_parser.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "model/book_delta.h"

namespace basis::feed {

namespace {

using model::Action;
using model::BookDelta;
using model::Side;
using model::Venue;

// Binance quantities are base units scaled by 1e8, the venue's own
// convention, which keeps the whole range in int64.
constexpr double kSizeScale = 1e8;

// A decimal string to integer cents, exact or not at all. Returning false
// rather than rounding is the point: a price off the cent grid cannot be
// represented in this engine's price model, and silently truncating it
// would put a wrong book in front of the analytics.
bool to_exact_cents(std::string_view text, int* out) {
  if (text.empty()) return false;
  char* end = nullptr;
  // simdjson hands back views into its own padded buffer, which is not
  // guaranteed null-terminated at the view's end; copy the few bytes a
  // price occupies rather than reading past it.
  char buf[64];
  if (text.size() >= sizeof(buf)) return false;
  std::memcpy(buf, text.data(), text.size());
  buf[text.size()] = '\0';
  const double value = std::strtod(buf, &end);
  if (end != buf + text.size()) return false;
  if (!std::isfinite(value) || value < 0.0) return false;
  const double cents = value * 100.0;
  const double rounded = std::nearbyint(cents);
  if (std::fabs(cents - rounded) > 1e-6) return false;  // off the cent grid
  if (rounded > 2'000'000'000.0) return false;          // beyond int cents
  *out = static_cast<int>(rounded);
  return true;
}

bool to_scaled_size(std::string_view text, std::int64_t* out) {
  if (text.empty()) return false;
  char buf[64];
  if (text.size() >= sizeof(buf)) return false;
  std::memcpy(buf, text.data(), text.size());
  buf[text.size()] = '\0';
  char* end = nullptr;
  const double value = std::strtod(buf, &end);
  if (end != buf + text.size()) return false;
  if (!std::isfinite(value) || value < 0.0) return false;
  const double scaled = std::nearbyint(value * kSizeScale);
  if (scaled > 9.0e18) return false;
  *out = static_cast<std::int64_t>(scaled);
  return true;
}

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
