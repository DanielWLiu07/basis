#include <gtest/gtest.h>

#include "normalize/crypto_instrument.h"

using basis::model::Venue;
using basis::normalize::canonical_instrument;

// The pairing this function performs is invisible when it works and silent
// when it fails: two venues that never match produce an empty result, which
// reads as a quiet market rather than a bug. So it is tested directly.

TEST(CryptoInstrument, PairsTheTwoVenuesSpellingsOfTheSameAsset) {
  EXPECT_EQ(canonical_instrument(Venue::Binance, "btcusdt"), "BTC/USD");
  EXPECT_EQ(canonical_instrument(Venue::Coinbase, "BTC-USD"), "BTC/USD");
  EXPECT_EQ(canonical_instrument(Venue::Binance, "ethusdt"), "ETH/USD");
  EXPECT_EQ(canonical_instrument(Venue::Coinbase, "ETH-USD"), "ETH/USD");
}

TEST(CryptoInstrument, IsCaseInsensitive) {
  EXPECT_EQ(canonical_instrument(Venue::Binance, "BTCUSDT"), "BTC/USD");
  EXPECT_EQ(canonical_instrument(Venue::Coinbase, "btc-usd"), "BTC/USD");
}

// The longest-suffix rule is the whole reason the quote table is ordered.
// Stripping "USD" from BTCUSDT would leave a base of "BTCUST", which is a
// wrong answer that looks like a right one.
TEST(CryptoInstrument, StripsTheLongestMatchingQuoteNotTheFirst) {
  EXPECT_EQ(canonical_instrument(Venue::Binance, "btcusdt"), "BTC/USD");
  EXPECT_EQ(canonical_instrument(Venue::Binance, "btcusdc"), "BTC/USD");
  EXPECT_EQ(canonical_instrument(Venue::Binance, "btcusd"),  "BTC/USD");
  EXPECT_EQ(canonical_instrument(Venue::Binance, "ethbusd"), "ETH/USD");
  EXPECT_EQ(canonical_instrument(Venue::Binance, "ethfdusd"), "ETH/USD");
  // A dollar stablecoin quoted against another one still has a base.
  EXPECT_EQ(canonical_instrument(Venue::Binance, "usdcusdt"), "USDC/USD");
}

// A non-dollar quote is refused rather than guessed. Pairing BTC-EUR
// against BTCUSDT would report a lead that is largely the EUR/USD rate.
TEST(CryptoInstrument, RefusesNonDollarQuotes) {
  EXPECT_FALSE(canonical_instrument(Venue::Coinbase, "BTC-EUR").has_value());
  EXPECT_FALSE(canonical_instrument(Venue::Coinbase, "BTC-GBP").has_value());
  EXPECT_FALSE(canonical_instrument(Venue::Binance, "btceth").has_value());
}

TEST(CryptoInstrument, RefusesMalformedSymbols) {
  EXPECT_FALSE(canonical_instrument(Venue::Binance, "").has_value());
  EXPECT_FALSE(canonical_instrument(Venue::Coinbase, "").has_value());
  // Coinbase products are hyphenated; an unhyphenated one is not a spot
  // pair, and must not fall through to the glued Binance parse.
  EXPECT_FALSE(canonical_instrument(Venue::Coinbase, "BTCUSD").has_value());
  // A quote with nothing in front of it has no base.
  EXPECT_FALSE(canonical_instrument(Venue::Binance, "usdt").has_value());
  EXPECT_FALSE(canonical_instrument(Venue::Coinbase, "-USD").has_value());
}
