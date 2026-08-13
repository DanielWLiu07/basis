#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "feed/coinbase_parser.h"
#include "feed/feed_adapter.h"
#include "net/ws_client.h"

namespace basis::feed {

// Live Coinbase Exchange market data (no auth).
//
// Subscribes on every (re)connect, and the venue answers with a fresh book
// snapshot, which doubles as gap recovery - the same shape as the
// Polymarket feed. The parser puts an Action::Clear ahead of each snapshot
// so a reconnect rebuilds the book rather than layering a new image over
// levels the new one happens not to mention.
//
// level2_batch is the default channel because it is the one that is both
// public and quote-driven: `level2` now requires authentication, and
// `ticker` only publishes on trades, which samples the book at trade times
// and is not the same thing as a quote stream. The cost is that
// level2_batch coalesces on a 50 ms timer, which is a floor on any
// timing resolved from it (docs/bench/cross_venue_lead.md).
class CoinbaseFeed final : public FeedAdapter {
 public:
  struct Config {
    std::vector<std::string> product_ids;  // e.g. {"BTC-USD"}
    std::string channel = "level2_batch";
    std::string host = "ws-feed.exchange.coinbase.com";
    std::string port = "443";
    std::string target = "/";
    std::string trusted_ca_pem;
    std::int64_t initial_backoff_ms = 500;
  };

  using RawTap = std::function<void(std::string_view, std::int64_t)>;

  explicit CoinbaseFeed(Config config);
  ~CoinbaseFeed() override;

  model::Venue venue() const override { return model::Venue::Coinbase; }
  void set_sink(DeltaSink sink) override { sink_ = std::move(sink); }
  void set_raw_tap(RawTap tap) { raw_tap_ = std::move(tap); }

  void start() override;
  void stop() override;

  std::uint64_t messages() const { return client_.messages(); }
  std::uint64_t bytes() const { return client_.bytes(); }
  std::uint64_t reconnects() const { return client_.reconnects(); }
  std::uint64_t malformed() const { return malformed_.load(); }
  std::uint64_t deltas() const { return deltas_.load(); }
  // Levels the venue published that this engine's price model cannot hold
  // (a real BTC-USD snapshot carries asks above $20,000,000). Counted, not
  // silently dropped.
  std::uint64_t unrepresentable() const { return unrepresentable_.load(); }

  // Exposed for the unit test, which checks the subscribe frame without
  // opening a socket.
  std::string subscribe_message() const;

 private:
  void on_message(std::string_view payload, std::int64_t recv_ns);

  Config config_;
  net::WsClient client_;
  CoinbaseParser parser_;
  DeltaSink sink_;
  RawTap raw_tap_;
  std::atomic<std::uint64_t> malformed_{0};
  std::atomic<std::uint64_t> deltas_{0};
  std::atomic<std::uint64_t> unrepresentable_{0};
};

}  // namespace basis::feed
