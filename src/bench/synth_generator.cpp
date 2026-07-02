#include "bench/synth_generator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "core/rng.h"
#include "feed/feed_log.h"

namespace basis::bench {

namespace {

constexpr std::int64_t kLevelSize = 100;  // contracts resting per quote level

std::string cents_to_decimal(int cents) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%d.%02d", cents / 100, cents % 100);
  return buf;
}

std::string kalshi_snapshot(const std::string& ticker, std::uint64_t seq,
                            int bid, int ask) {
  char buf[256];
  // The no side holds bids on the NO contract: a NO bid at 100 - ask is the
  // YES ask our parser folds back, so the round trip exercises the folding.
  std::snprintf(buf, sizeof(buf),
                R"({"type":"orderbook_snapshot","sid":1,"seq":%llu,"msg":{)"
                R"("market_ticker":"%s","yes":[[%d,%lld]],"no":[[%d,%lld]]}})",
                static_cast<unsigned long long>(seq), ticker.c_str(), bid,
                static_cast<long long>(kLevelSize), 100 - ask,
                static_cast<long long>(kLevelSize));
  return buf;
}

std::string kalshi_delta(const std::string& ticker, std::uint64_t seq,
                         const char* side, int price, std::int64_t change) {
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                R"({"type":"orderbook_delta","sid":1,"seq":%llu,"msg":{)"
                R"("market_ticker":"%s","price":%d,"delta":%lld,"side":"%s"}})",
                static_cast<unsigned long long>(seq), ticker.c_str(), price,
                static_cast<long long>(change), side);
  return buf;
}

std::string polymarket_book(const std::string& token, int bid, int ask,
                            std::int64_t ts_ms) {
  char buf[512];
  std::snprintf(
      buf, sizeof(buf),
      R"({"event_type":"book","asset_id":"%s","market":"0xsynthetic",)"
      R"("bids":[{"price":"%s","size":"%lld"}],)"
      R"("asks":[{"price":"%s","size":"%lld"}],)"
      R"("timestamp":"%lld","hash":"synthetic"})",
      token.c_str(), cents_to_decimal(bid).c_str(),
      static_cast<long long>(kLevelSize), cents_to_decimal(ask).c_str(),
      static_cast<long long>(kLevelSize), static_cast<long long>(ts_ms));
  return buf;
}

std::string polymarket_change(const std::string& token, const char* side,
                              int price, std::int64_t size,
                              std::int64_t ts_ms) {
  char buf[512];
  std::snprintf(
      buf, sizeof(buf),
      R"({"event_type":"price_change","market":"0xsynthetic",)"
      R"("price_changes":[{"asset_id":"%s","price":"%s","size":"%lld",)"
      R"("side":"%s","hash":"synthetic"}],"timestamp":"%lld"})",
      token.c_str(), cents_to_decimal(price).c_str(),
      static_cast<long long>(size), side, static_cast<long long>(ts_ms));
  return buf;
}

}  // namespace

bool generate_synthetic_session(const SynthConfig& config,
                                const std::string& path, std::string* error) {
  // Portable draws (core/rng.h): the same seed yields the same file on
  // every platform, which is what "deterministic session" promises.
  std::mt19937 engine(config.seed);
  const auto move = [&engine] { return rng::normal(engine, 0.0, 0.6); };
  const auto jitter = [&engine] {
    return rng::uniform_int(engine, -20'000'000, 20'000'000);
  };

  std::vector<feed::FeedLogRecord> records;
  records.reserve(static_cast<std::size_t>(config.steps) * 4 + 8);

  double latent = 50.0;
  int bid = 49;
  int ask = 51;
  std::uint64_t seq = 1;

  // Opening snapshots on both venues, Polymarket trailing by the lead.
  records.push_back({config.start_ts_ns, model::Venue::Kalshi,
                     kalshi_snapshot(config.kalshi_ticker, seq, bid, ask)});
  records.push_back(
      {config.start_ts_ns + config.lead_ns, model::Venue::Polymarket,
       polymarket_book(config.polymarket_token, bid, ask,
                       (config.start_ts_ns + config.lead_ns) / 1'000'000)});

  for (int step = 1; step <= config.steps; ++step) {
    latent = std::clamp(latent + move(), 5.0, 95.0);
    const int new_bid = static_cast<int>(std::lround(latent)) - 1;
    const int new_ask = new_bid + 2;
    if (new_bid == bid) continue;  // quote unchanged, no messages

    const std::int64_t ts =
        config.start_ts_ns +
        static_cast<std::int64_t>(step) * config.step_ns + jitter();

    // Kalshi moves first: pull the old level, post the new one, per side.
    // The YES frame's ask lives on the wire as a NO bid at 100 - ask.
    records.push_back({ts, model::Venue::Kalshi,
                       kalshi_delta(config.kalshi_ticker, ++seq, "yes", bid,
                                    -kLevelSize)});
    records.push_back({ts, model::Venue::Kalshi,
                       kalshi_delta(config.kalshi_ticker, ++seq, "yes",
                                    new_bid, kLevelSize)});
    records.push_back({ts, model::Venue::Kalshi,
                       kalshi_delta(config.kalshi_ticker, ++seq, "no",
                                    100 - ask, -kLevelSize)});
    records.push_back({ts, model::Venue::Kalshi,
                       kalshi_delta(config.kalshi_ticker, ++seq, "no",
                                    100 - new_ask, kLevelSize)});

    // Polymarket posts the same quote change lead_ns later, in its own
    // dialect: absolute sizes, decimal prices, BUY/SELL.
    const std::int64_t poly_ts = ts + config.lead_ns;
    const std::int64_t poly_ms = poly_ts / 1'000'000;
    records.push_back({poly_ts, model::Venue::Polymarket,
                       polymarket_change(config.polymarket_token, "BUY", bid,
                                         0, poly_ms)});
    records.push_back({poly_ts, model::Venue::Polymarket,
                       polymarket_change(config.polymarket_token, "BUY",
                                         new_bid, kLevelSize, poly_ms)});
    records.push_back({poly_ts, model::Venue::Polymarket,
                       polymarket_change(config.polymarket_token, "SELL", ask,
                                         0, poly_ms)});
    records.push_back({poly_ts, model::Venue::Polymarket,
                       polymarket_change(config.polymarket_token, "SELL",
                                         new_ask, kLevelSize, poly_ms)});

    bid = new_bid;
    ask = new_ask;
  }

  // Interleave into arrival order; the Polymarket echo of one move lands
  // among later Kalshi moves, exactly like a live capture would.
  std::stable_sort(records.begin(), records.end(),
                   [](const feed::FeedLogRecord& a,
                      const feed::FeedLogRecord& b) {
                     return a.recv_ns < b.recv_ns;
                   });

  feed::FeedLogWriter writer(path);
  if (!writer.ok()) {
    if (error) *error = "cannot open " + path + " for writing";
    return false;
  }
  for (const auto& record : records) {
    if (!writer.write(record)) {
      if (error) *error = "write failed on " + path;
      return false;
    }
  }
  return true;
}

}  // namespace basis::bench
