#include "feed/polymarket_feed.h"

#include "core/logger.h"

namespace basis::feed {

PolymarketFeed::PolymarketFeed(Config config)
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

PolymarketFeed::~PolymarketFeed() { stop(); }

void PolymarketFeed::start() {
  if (config_.token_ids.empty()) {
    log::warn("polymarket feed started with no tokens configured");
  }
  client_.start();
}

void PolymarketFeed::stop() { client_.stop(); }

std::string PolymarketFeed::subscribe_message() const {
  // {"assets_ids":["<token>",...],"type":"market"}; token ids are decimal
  // digit strings, so no JSON escaping is needed.
  std::string msg = R"({"assets_ids":[)";
  for (std::size_t i = 0; i < config_.token_ids.size(); ++i) {
    if (i > 0) msg += ',';
    msg += '"';
    msg += config_.token_ids[i];
    msg += '"';
  }
  msg += R"(],"type":"market"})";
  return msg;
}

void PolymarketFeed::on_message(std::string_view payload,
                                std::int64_t recv_ns) {
  if (raw_tap_) raw_tap_(payload, recv_ns);

  const auto parsed = parser_.parse(payload, recv_ns);
  if (parsed.status == ParseStatus::Malformed) {
    ++malformed_;
    return;
  }
  hashes_verified_ += parsed.hashes_verified;
  if (parsed.hashes_mismatched > 0) {
    // A book whose recomputed hash disagrees with the venue's is corrupt
    // somewhere between their matching engine and this process. Counted
    // and shouted; the periodic snapshots will replace the state either
    // way, so no automated recovery fires from here.
    hashes_mismatched_ += parsed.hashes_mismatched;
    log::warn("polymarket feed: snapshot hash mismatch");
  }
  deltas_ += parsed.deltas.size();
  if (!sink_) return;
  for (const auto& delta : parsed.deltas) sink_(delta);
}

}  // namespace basis::feed
