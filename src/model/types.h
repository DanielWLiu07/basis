#pragma once

#include <cstdint>

namespace basis::model {

enum class Venue : std::uint8_t { Kalshi, Polymarket };
enum class Side : std::uint8_t { Bid, Ask };

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
