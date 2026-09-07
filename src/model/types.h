#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace basis::model {

// Venues the engine can normalize into one book. The prediction markets
// are the subject; Binance is here because it produces load the others
// cannot (docs/bench/ingest.md), and Coinbase because it quotes the same
// instrument as Binance with no credentials, which is what makes a real
// cross-venue lead measurable (docs/bench/cross_venue_lead.md).
enum class Venue : std::uint8_t { Kalshi, Polymarket, Binance, Coinbase };
inline constexpr int kVenueCount = 4;
enum class Side : std::uint8_t { Bid, Ask };

// Which side crossed the spread to cause a trade.
//
// Unknown is a real state, not a default to be tidied away: Binance's
// aggTrade reports a maker flag, Coinbase's ticker reports the maker's
// side, and a venue that publishes neither leaves this genuinely
// undetermined. Order-flow imbalance built on a guessed aggressor is
// worse than one that reports coverage honestly, so the parsers set this
// only when the wire says so.
enum class Aggressor : std::uint8_t { Unknown, Buy, Sell };

constexpr const char* to_string(Aggressor a) {
  switch (a) {
    case Aggressor::Unknown: return "unknown";
    case Aggressor::Buy:     return "buy";
    case Aggressor::Sell:    return "sell";
  }
  return "?";
}

inline std::optional<Venue> venue_from_string(std::string_view s) {
  if (s == "kalshi") return Venue::Kalshi;
  if (s == "polymarket") return Venue::Polymarket;
  if (s == "binance") return Venue::Binance;
  if (s == "coinbase") return Venue::Coinbase;
  return std::nullopt;
}

constexpr const char* to_string(Venue v) {
  switch (v) {
    case Venue::Kalshi:     return "kalshi";
    case Venue::Polymarket: return "polymarket";
    case Venue::Binance:    return "binance";
    case Venue::Coinbase:   return "coinbase";
  }
  return "?";
}

constexpr const char* to_string(Side s) {
  return s == Side::Bid ? "bid" : "ask";
}

}  // namespace basis::model
