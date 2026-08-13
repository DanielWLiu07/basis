#include <gtest/gtest.h>

#include "feed_live/binance_feed.h"
#include "feed_live/coinbase_feed.h"
#include "model/types.h"

using basis::feed::BinanceFeed;
using basis::feed::CoinbaseFeed;
using basis::model::Venue;

// The subscription each venue receives is the part of a live adapter that
// can be wrong without any socket being involved, and the part a venue
// answers with silence rather than an error. Both are checked here; the
// reconnect and TLS behaviour they share with the other feeds is covered
// against a local server in test_reconnect.cpp.

TEST(BinanceFeed, StreamListRidesInTheUrlSoReconnectResubscribes) {
  // Combined-stream form. Putting the streams in the target rather than in
  // a subscribe frame means a dropped connection comes back subscribed,
  // with no ack to race.
  EXPECT_EQ(BinanceFeed::stream_target({"btcusdt"}),
            "/stream?streams=btcusdt@bookTicker");
  EXPECT_EQ(BinanceFeed::stream_target({"btcusdt", "ethusdt"}),
            "/stream?streams=btcusdt@bookTicker/ethusdt@bookTicker");
}

TEST(BinanceFeed, ReportsItsVenue) {
  BinanceFeed feed({.symbols = {"btcusdt"}});
  EXPECT_EQ(feed.venue(), Venue::Binance);
  EXPECT_EQ(feed.messages(), 0u);
  EXPECT_EQ(feed.malformed(), 0u);
}

TEST(CoinbaseFeed, SubscribeFrameNamesProductsAndChannel) {
  CoinbaseFeed one({.product_ids = {"BTC-USD"}});
  EXPECT_EQ(one.subscribe_message(),
            R"({"type":"subscribe","product_ids":["BTC-USD"],)"
            R"("channels":["level2_batch"]})");

  CoinbaseFeed many({.product_ids = {"BTC-USD", "ETH-USD"}});
  EXPECT_EQ(many.subscribe_message(),
            R"({"type":"subscribe","product_ids":["BTC-USD","ETH-USD"],)"
            R"("channels":["level2_batch"]})");
}

// level2_batch is the default for a reason worth pinning: `level2` now
// requires authentication, and `ticker` only publishes on trades, which
// samples the book at trade times instead of following quotes.
TEST(CoinbaseFeed, DefaultsToThePublicQuoteDrivenChannel) {
  CoinbaseFeed feed({.product_ids = {"BTC-USD"}});
  EXPECT_NE(feed.subscribe_message().find("level2_batch"),
            std::string::npos);
  EXPECT_EQ(feed.venue(), Venue::Coinbase);
}

TEST(CoinbaseFeed, ChannelIsOverridable) {
  CoinbaseFeed feed({.product_ids = {"BTC-USD"}, .channel = "ticker"});
  EXPECT_NE(feed.subscribe_message().find(R"("channels":["ticker"])"),
            std::string::npos);
}
