#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "feed/binance_parser.h"
#include "feed/feed_adapter.h"
#include "net/ws_client.h"

namespace basis::feed {

// Live Binance market data over the public combined stream (no auth).
//
// This exists to close a gap rather than to add a venue. The cross-venue
// lead in docs/bench/cross_venue_lead.md was captured by a Python script,
// which is a strange thing for a C++ market-data engine to need: the same
// TLS WebSocket client already carries Kalshi and Polymarket. With this
// and CoinbaseFeed, `basis record` captures the data its own analysis
// reads, and the capture path is the code under test rather than a
// separate script that could drift from it.
//
// Subscription is by URL rather than by message: the combined-stream
// endpoint takes the stream list in the target
// (/stream?streams=btcusdt@bookTicker/ethusdt@bookTicker), so a reconnect
// re-subscribes for free and there is no subscribe-ack to race. Note that
// '!bookTicker' silently delivers nothing on combined streams; the
// per-symbol form is the one that works.
class BinanceFeed final : public FeedAdapter {
 public:
  struct Config {
    // Lowercase symbols, e.g. {"btcusdt"}. Each becomes <sym>@bookTicker.
    std::vector<std::string> symbols;
    std::string host = "stream.binance.com";
    std::string port = "9443";
    std::string trusted_ca_pem;
    std::int64_t initial_backoff_ms = 500;
  };

  using RawTap = std::function<void(std::string_view, std::int64_t)>;

  explicit BinanceFeed(Config config);
  ~BinanceFeed() override;

  model::Venue venue() const override { return model::Venue::Binance; }
  void set_sink(DeltaSink sink) override { sink_ = std::move(sink); }
  void set_raw_tap(RawTap tap) { raw_tap_ = std::move(tap); }

  void start() override;
  void stop() override;

  std::uint64_t messages() const { return client_.messages(); }
  std::uint64_t bytes() const { return client_.bytes(); }
  std::uint64_t reconnects() const { return client_.reconnects(); }
  std::uint64_t malformed() const { return malformed_.load(); }
  std::uint64_t deltas() const { return deltas_.load(); }

  // Exposed so a caller can build the same URL without connecting, which
  // is what the unit test checks.
  static std::string stream_target(const std::vector<std::string>& symbols);

 private:
  void on_message(std::string_view payload, std::int64_t recv_ns);

  Config config_;
  net::WsClient client_;
  BinanceParser parser_;
  DeltaSink sink_;
  RawTap raw_tap_;
  std::atomic<std::uint64_t> malformed_{0};
  std::atomic<std::uint64_t> deltas_{0};
};

}  // namespace basis::feed
