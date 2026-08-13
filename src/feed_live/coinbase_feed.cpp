#include "feed_live/coinbase_feed.h"

#include "core/logger.h"

namespace basis::feed {

CoinbaseFeed::CoinbaseFeed(Config config)
    : config_(std::move(config)),
      client_(net::WsConfig{
          .host = config_.host,
          .port = config_.port,
          .target = config_.target,
          .trusted_ca_pem = config_.trusted_ca_pem,
          .initial_backoff_ms = config_.initial_backoff_ms}) {
  client_.set_on_connect([this](net::WsClient& client) {
    client.send(subscribe_message());
  });
  client_.set_on_message([this](std::string_view payload,
                                std::int64_t recv_ns) {
    on_message(payload, recv_ns);
  });
}

CoinbaseFeed::~CoinbaseFeed() { stop(); }

void CoinbaseFeed::start() {
  if (config_.product_ids.empty()) {
    log::warn("coinbase feed started with no products configured");
  }
  client_.start();
}

void CoinbaseFeed::stop() { client_.stop(); }

std::string CoinbaseFeed::subscribe_message() const {
  // {"type":"subscribe","product_ids":["BTC-USD"],"channels":["level2_batch"]}
  // Product ids and channel names are venue-defined identifiers drawn from
  // [A-Z0-9-_], so no JSON escaping is needed.
  std::string msg = R"({"type":"subscribe","product_ids":[)";
  for (std::size_t i = 0; i < config_.product_ids.size(); ++i) {
    if (i > 0) msg += ',';
    msg += '"';
    msg += config_.product_ids[i];
    msg += '"';
  }
  msg += R"(],"channels":[")";
  msg += config_.channel;
  msg += R"("]})";
  return msg;
}

void CoinbaseFeed::on_message(std::string_view payload,
                              std::int64_t recv_ns) {
  if (raw_tap_) raw_tap_(payload, recv_ns);

  const auto parsed = parser_.parse(payload, recv_ns);
  unrepresentable_ += parsed.levels_unrepresentable;
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
