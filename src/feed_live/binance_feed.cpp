#include "feed_live/binance_feed.h"

#include "core/logger.h"

namespace basis::feed {

std::string BinanceFeed::stream_target(
    const std::vector<std::string>& symbols) {
  std::string target = "/stream?streams=";
  for (std::size_t i = 0; i < symbols.size(); ++i) {
    if (i > 0) target += '/';
    target += symbols[i];
    target += "@bookTicker";
  }
  return target;
}

BinanceFeed::BinanceFeed(Config config)
    : config_(std::move(config)),
      client_(net::WsConfig{
          .host = config_.host,
          .port = config_.port,
          .target = stream_target(config_.symbols),
          .trusted_ca_pem = config_.trusted_ca_pem,
          .initial_backoff_ms = config_.initial_backoff_ms}) {
  // No on_connect handler: the stream list rides in the URL, so a
  // reconnect resubscribes by definition.
  client_.set_on_message([this](std::string_view payload,
                                std::int64_t recv_ns) {
    on_message(payload, recv_ns);
  });
}

BinanceFeed::~BinanceFeed() { stop(); }

void BinanceFeed::start() {
  if (config_.symbols.empty()) {
    log::warn("binance feed started with no symbols configured");
  }
  client_.start();
}

void BinanceFeed::stop() { client_.stop(); }

void BinanceFeed::on_message(std::string_view payload,
                             std::int64_t recv_ns) {
  if (raw_tap_) raw_tap_(payload, recv_ns);

  const auto parsed = parser_.parse(payload, recv_ns);
  if (parsed.status == ParseStatus::Malformed) {
    ++malformed_;
    return;
  }
  if (!sink_) return;
  for (const auto& delta : parsed.deltas) {
    sink_(delta);
    ++deltas_;
  }
}

}  // namespace basis::feed
