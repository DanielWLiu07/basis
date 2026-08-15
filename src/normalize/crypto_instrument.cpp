#include "normalize/crypto_instrument.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace basis::normalize {
namespace {

// Dollar-like quotes, longest first so BTCUSDT strips USDT and not USD.
// Order matters: matching USD against "BTCUSDT" would leave a base of
// "BTCUST", which is not a symbol anyone would notice was wrong.
constexpr std::array<std::string_view, 5> kUsdQuotes = {
    "FDUSD", "BUSD", "USDT", "USDC", "USD"};

std::string upper(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return out;
}

bool is_alnum_symbol(std::string_view s) {
  if (s.empty()) return false;
  return std::all_of(s.begin(), s.end(), [](unsigned char c) {
    return std::isalnum(c) != 0;
  });
}

}  // namespace

std::optional<std::string> canonical_instrument(model::Venue venue,
                                                std::string_view market) {
  if (market.empty()) return std::nullopt;
  const std::string sym = upper(market);

  std::string base;
  if (venue == model::Venue::Coinbase) {
    // "BTC-USD": split on the separator. A product id without one is not a
    // Coinbase spot pair, so refuse rather than fall through to the glued
    // parse and invent a base.
    const auto dash = sym.find('-');
    if (dash == std::string::npos) return std::nullopt;
    const std::string quote = sym.substr(dash + 1);
    if (std::find(kUsdQuotes.begin(), kUsdQuotes.end(), quote) ==
        kUsdQuotes.end()) {
      return std::nullopt;
    }
    base = sym.substr(0, dash);
  } else {
    // "BTCUSDT": no separator, so the quote has to be recognised by suffix.
    for (const auto quote : kUsdQuotes) {
      if (sym.size() > quote.size() &&
          sym.compare(sym.size() - quote.size(), quote.size(), quote) == 0) {
        base = sym.substr(0, sym.size() - quote.size());
        break;
      }
    }
    if (base.empty()) return std::nullopt;
  }

  if (!is_alnum_symbol(base)) return std::nullopt;
  return base + "/USD";
}

}  // namespace basis::normalize
