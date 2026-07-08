#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "feed/feed_adapter.h"
#include "feed/polymarket_parser.h"
#include "net/ws_client.h"

namespace basis::feed {

// Live Polymarket order books over the public market WebSocket (no auth,
// docs/api_integration.md). Subscribes to the configured outcome tokens on
// every (re)connect; the server answers with a fresh book snapshot, which
// doubles as gap recovery. Deltas reach the sink on the IO thread.
class PolymarketFeed final : public FeedAdapter {
 public:
  struct Config {
    std::vector<std::string> token_ids;  // outcome tokens (asset ids)
    std::string host = "ws-subscriptions-clob.polymarket.com";
    std::string port = "443";
    std::string target = "/ws/market";
    // Passed through to the WebSocket client; the defaults are for the
    // real venue, the overrides are how the fault-injection test points
    // the same feed stack at a local server.
    std::string trusted_ca_pem;
    std::int64_t initial_backoff_ms = 500;
  };

  // Raw tap for the record tool: every wire message verbatim, before
  // parsing, with its receive timestamp. Runs on the IO thread.
  using RawTap = std::function<void(std::string_view, std::int64_t)>;

  explicit PolymarketFeed(Config config);
  ~PolymarketFeed() override;

  model::Venue venue() const override { return model::Venue::Polymarket; }
  void set_sink(DeltaSink sink) override { sink_ = std::move(sink); }
  void set_raw_tap(RawTap tap) { raw_tap_ = std::move(tap); }

  void start() override;
  void stop() override;

  std::uint64_t messages() const { return client_.messages(); }
  std::uint64_t bytes() const { return client_.bytes(); }
  std::uint64_t reconnects() const { return client_.reconnects(); }
  std::uint64_t malformed() const { return malformed_; }
  std::uint64_t deltas() const { return deltas_; }
  std::uint64_t hashes_verified() const { return hashes_verified_; }
  std::uint64_t hashes_mismatched() const { return hashes_mismatched_; }

 private:
  std::string subscribe_message() const;
  void on_message(std::string_view payload, std::int64_t recv_ns);

  Config config_;
  net::WsClient client_;
  PolymarketParser parser_;
  DeltaSink sink_;
  RawTap raw_tap_;
  std::uint64_t malformed_ = 0;          // IO-thread only
  std::uint64_t deltas_ = 0;             // IO-thread only
  std::uint64_t hashes_verified_ = 0;    // IO-thread only
  std::uint64_t hashes_mismatched_ = 0;  // IO-thread only
};

}  // namespace basis::feed
