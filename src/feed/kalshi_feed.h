#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "feed/feed_adapter.h"
#include "feed/kalshi_parser.h"
#include "net/kalshi_auth.h"
#include "net/ws_client.h"

namespace basis::feed {

// Live Kalshi order books over the authenticated trade-api WebSocket
// (docs/api_integration.md). Every connection re-signs the auth headers
// (the signature embeds a timestamp) and re-subscribes; the server answers
// a subscribe with a full snapshot per market. On a sequence gap the local
// books are already cleared by the parser, and the feed re-subscribes to
// force fresh snapshots rather than trusting an incremental repair.
// Deltas reach the sink on the IO thread.
class KalshiFeed final : public FeedAdapter {
 public:
  struct Config {
    std::vector<std::string> market_tickers;
    std::string key_id;
    net::KalshiSigner signer;
    std::string host = "external-api-ws.kalshi.com";
    std::string target = "/trade-api/ws/v2";
  };

  // Raw tap for the record tool: every wire message verbatim, before
  // parsing, with its receive timestamp. Runs on the IO thread.
  using RawTap = std::function<void(std::string_view, std::int64_t)>;

  explicit KalshiFeed(Config config);
  ~KalshiFeed() override;

  model::Venue venue() const override { return model::Venue::Kalshi; }
  void set_sink(DeltaSink sink) override { sink_ = std::move(sink); }
  void set_raw_tap(RawTap tap) { raw_tap_ = std::move(tap); }

  void start() override;
  void stop() override;

  std::uint64_t messages() const { return client_.messages(); }
  std::uint64_t bytes() const { return client_.bytes(); }
  std::uint64_t reconnects() const { return client_.reconnects(); }
  std::uint64_t malformed() const { return malformed_; }
  std::uint64_t deltas() const { return deltas_; }
  std::uint64_t gaps() const { return gaps_; }

 private:
  std::string subscribe_message();
  void on_message(std::string_view payload, std::int64_t recv_ns);

  Config config_;
  net::WsClient client_;
  KalshiParser parser_;
  DeltaSink sink_;
  RawTap raw_tap_;
  std::uint64_t malformed_ = 0;     // IO-thread only
  std::uint64_t deltas_ = 0;        // IO-thread only
  std::uint64_t gaps_ = 0;          // IO-thread only
  std::uint64_t next_command_id_ = 1;  // IO-thread only
};

}  // namespace basis::feed
