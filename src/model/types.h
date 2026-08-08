#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace basis::model {

// Venues the engine can normalize into one book. The prediction markets
// are the subject; Binance is here because it produces load the others
// cannot (docs/bench/ingest.md).
enum class Venue : std::uint8_t { Kalshi, Polymarket, Binance };
inline constexpr int kVenueCount = 3;
enum class Side : std::uint8_t { Bid, Ask };

inline std::optional<Venue> venue_from_string(std::string_view s) {
  if (s == "kalshi") return Venue::Kalshi;
  if (s == "polymarket") return Venue::Polymarket;
  if (s == "binance") return Venue::Binance;
  return std::nullopt;
}

constexpr const char* to_string(Venue v) {
  switch (v) {
    case Venue::Kalshi:     return "kalshi";
    case Venue::Polymarket: return "polymarket";
    case Venue::Binance:    return "binance";
  }
  return "?";
}

constexpr const char* to_string(Side s) {
  return s == Side::Bid ? "bid" : "ask";
}

}  // namespace basis::model
