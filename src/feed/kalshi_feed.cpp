#include "feed/kalshi_feed.h"

#include "core/logger.h"
#include "core/time.h"

namespace basis::feed {

KalshiFeed::KalshiFeed(Config config)
    : config_(std::move(config)),
      client_(net::WsConfig{.host = config_.host, .target = config_.target}) {
  // Auth headers are minted per connect: the signature covers a timestamp,
  // so the set built for the first handshake is invalid for a reconnect.
  client_.set_header_provider([this] {
    return config_.signer.ws_headers(config_.key_id, config_.target,
                                     time::wall_ns() / 1'000'000);
  });
  client_.set_on_connect([this](net::WsClient& client) {
    client.send(subscribe_message());
  });
  client_.set_on_message([this](std::string_view payload,
                                std::int64_t recv_ns) {
    on_message(payload, recv_ns);
  });
}

KalshiFeed::~KalshiFeed() { stop(); }

void KalshiFeed::start() {
  if (config_.market_tickers.empty()) {
    log::warn("kalshi feed started with no tickers configured");
  }
  client_.start();
}

void KalshiFeed::stop() { client_.stop(); }

std::string KalshiFeed::subscribe_message() {
  // {"id":N,"cmd":"subscribe","params":{"channels":["orderbook_delta"],
  //  "market_tickers":["T",...]}}; tickers are [A-Z0-9-] strings, so no
  // JSON escaping is needed.
  std::string msg = R"({"id":)";
  msg += std::to_string(next_command_id_++);
  msg += R"(,"cmd":"subscribe","params":{"channels":["orderbook_delta"],)";
  msg += R"("market_tickers":[)";
  for (std::size_t i = 0; i < config_.market_tickers.size(); ++i) {
    if (i > 0) msg += ',';
    msg += '"';
    msg += config_.market_tickers[i];
    msg += '"';
  }
  msg += R"(]}})";
  return msg;
}

void KalshiFeed::on_message(std::string_view payload, std::int64_t recv_ns) {
  if (raw_tap_) raw_tap_(payload, recv_ns);

  const auto parsed = parser_.parse(payload, recv_ns);
  if (parsed.status == ParseStatus::Malformed) {
    ++malformed_;
    return;
  }

  if (parsed.gap) {
    // The parser has already cleared the affected book; recovery is a
    // fresh snapshot, which the server sends on subscribe. A live
    // subscription cannot be subscribed again, so drop it first.
    ++gaps_;
    log::warn("kalshi feed: sequence gap, re-subscribing for a snapshot");
    if (parsed.sid != 0) {
      std::string unsubscribe = R"({"id":)";
      unsubscribe += std::to_string(next_command_id_++);
      unsubscribe += R"(,"cmd":"unsubscribe","params":{"sids":[)";
      unsubscribe += std::to_string(parsed.sid);
      unsubscribe += R"(]}})";
      client_.send(unsubscribe);
    }
    client_.send(subscribe_message());
  }

  deltas_ += parsed.deltas.size();
  if (!sink_) return;
  for (const auto& delta : parsed.deltas) sink_(delta);
}

}  // namespace basis::feed
