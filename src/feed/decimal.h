#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace basis::feed {

// Venue quantities are base units scaled by 1e8, which is Binance's own
// convention and keeps the whole range inside int64. Coinbase quotes sizes
// in the same shape (a decimal string of base units), so both venues share
// the scale and a size is comparable across them.
inline constexpr double kSizeScale = 1e8;

// Why a price failed to convert. Callers that can localize the damage to
// one level (a deep level in a book snapshot) treat OutOfRange as a level
// to skip and count; callers where the level IS the message (a top-of-book
// tick) still reject the whole thing, because dropping it would leave a
// stale best price standing as if it were current.
enum class PriceParse {
  Ok,
  NotANumber,   // not a decimal string, or negative
  OffGrid,      // finer than a cent; representing it would mean truncating
  OutOfRange,   // beyond int32 cents; no representation exists at all
};

// A decimal string to integer cents, exact or not at all. Refusing to round
// is the point: a price this engine cannot represent exactly must not be
// quietly turned into a nearby one it can, because that puts a wrong book
// in front of the analytics.
inline PriceParse parse_cents(std::string_view text, int* out) {
  if (text.empty()) return PriceParse::NotANumber;
  // simdjson hands back views into its own padded buffer, which is not
  // guaranteed null-terminated at the view's end; copy the few bytes a
  // price occupies rather than reading past it.
  char buf[64];
  if (text.size() >= sizeof(buf)) return PriceParse::NotANumber;
  std::memcpy(buf, text.data(), text.size());
  buf[text.size()] = '\0';
  char* end = nullptr;
  const double value = std::strtod(buf, &end);
  if (end != buf + text.size()) return PriceParse::NotANumber;
  if (!std::isfinite(value) || value < 0.0) return PriceParse::NotANumber;
  const double cents = value * 100.0;
  const double rounded = std::nearbyint(cents);
  if (std::fabs(cents - rounded) > 1e-6) return PriceParse::OffGrid;
  if (rounded > 2'000'000'000.0) return PriceParse::OutOfRange;
  *out = static_cast<int>(rounded);
  return PriceParse::Ok;
}

// The all-or-nothing form, for callers where one bad level means the whole
// message is unusable.
inline bool to_exact_cents(std::string_view text, int* out) {
  return parse_cents(text, out) == PriceParse::Ok;
}

inline bool to_scaled_size(std::string_view text, std::int64_t* out) {
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

// Kalshi quotes contract counts, and since its 2026 wire change they arrive
// as decimal strings that can be fractional and signed: `delta_fp` is
// "-2.83" when a resting size shrinks. Rounds to the nearest whole
// contract, which is the unit the rest of the engine counts Kalshi depth
// in - deliberately not kSizeScale, because a Kalshi contract is a countable
// thing, not a fractional base unit like a Binance quantity.
inline bool to_rounded_contracts(std::string_view text, std::int64_t* out) {
  if (text.empty()) return false;
  char buf[64];
  if (text.size() >= sizeof(buf)) return false;
  std::memcpy(buf, text.data(), text.size());
  buf[text.size()] = '\0';
  char* end = nullptr;
  const double value = std::strtod(buf, &end);
  if (end != buf + text.size()) return false;
  if (!std::isfinite(value)) return false;
  const double rounded = std::nearbyint(value);
  if (std::fabs(rounded) > 9.0e18) return false;
  *out = static_cast<std::int64_t>(rounded);
  return true;
}

}  // namespace basis::feed
