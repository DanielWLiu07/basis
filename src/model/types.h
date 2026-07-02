#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace basis::model {

enum class Venue : std::uint8_t { Kalshi, Polymarket };
enum class Side : std::uint8_t { Bid, Ask };

inline std::optional<Venue> venue_from_string(std::string_view s) {
  if (s == "kalshi") return Venue::Kalshi;
  if (s == "polymarket") return Venue::Polymarket;
  return std::nullopt;
}

constexpr const char* to_string(Venue v) {
  switch (v) {
    case Venue::Kalshi:     return "kalshi";
    case Venue::Polymarket: return "polymarket";
  }
  return "?";
}

constexpr const char* to_string(Side s) {
  return s == Side::Bid ? "bid" : "ask";
}

}  // namespace basis::model
