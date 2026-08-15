#include "cli/commands.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "analytics/divergence.h"
#include "analytics/lead_lag.h"
#include "cli/args.h"
#include "cli/usage.h"
#include "core/bounded_queue.h"
#include "core/logger.h"
#include "feed/feed_log.h"
#include "feed_live/binance_feed.h"
#include "feed_live/coinbase_feed.h"
#include "feed_live/kalshi_feed.h"
#include "feed_live/polymarket_feed.h"
#include "model/unified_book.h"
#include "net/kalshi_auth.h"
#include "normalize/contract_registry.h"
#include "normalize/normalizer.h"

namespace basis::cli {
namespace {

// Ctrl-C stops a capture without discarding it: the feeds are asked to
// stop and the log is closed on the way out, so an interrupted run leaves
// a readable capture rather than a truncated one.
std::atomic<bool> g_interrupted{false};

void handle_sigint(int) { g_interrupted.store(true); }

}  // namespace

int run_record(const std::vector<std::string_view>& args) {
  if (args.empty()) return usage();
  const std::string out_path(args[0]);
  const auto config_path =
      flag_string(args, "--config", "configs/contracts.toml");
  const auto seconds = flag_value(args, "--seconds", 0);  // 0: until ctrl-c
  if (!seconds || *seconds < 0 || *seconds > 7 * 86'400) {
    basis::log::error("record: bad --seconds value");
    return usage();
  }
  const auto binance_symbols = split_csv(flag_string(args, "--binance", ""));
  const auto coinbase_products =
      split_csv(flag_string(args, "--coinbase", ""));
  const bool crypto_only = !binance_symbols.empty() || !coinbase_products.empty();
  const auto kalshi_key_id = flag_string(args, "--kalshi-key-id", "");
  const auto kalshi_pem =
      flag_string(args, "--kalshi-pem", "secrets/kalshi.pem");

  std::string error;
  const auto registry =
      basis::normalize::TomlContractRegistry::load(config_path, &error);
  if (!registry) {
    basis::log::error(error);
    return 1;
  }
  const auto& tokens = registry->polymarket_tokens();
  const auto& tickers = registry->kalshi_tickers();
  if (tokens.empty() && !crypto_only) {
    basis::log::error("no polymarket tokens in " + config_path +
                      "; nothing to record");
    return 1;
  }

  basis::feed::FeedLogWriter writer(out_path);
  if (!writer.ok()) {
    basis::log::error("cannot open " + out_path + " for writing");
    return 1;
  }

  // The raw taps run on each feed's IO thread; the mutex serializes the
  // two writers into one feedlog. Rejected writes are counted, per the
  // no-silent-drop rule.
  std::mutex writer_mutex;
  std::atomic<std::uint64_t> written{0};
  std::atomic<std::uint64_t> rejected{0};
  const auto make_tap = [&](basis::model::Venue venue) {
    return [&, venue](std::string_view payload, std::int64_t recv_ns) {
      std::string framed(payload);
      // Some messages arrive with embedded newlines; outside JSON strings
      // whitespace is insignificant, so flattening keeps the payload valid
      // instead of losing a real message to the line framing.
      for (char& c : framed) {
        if (c == '\n' || c == '\r') c = ' ';
      }
      bool ok = false;
      {
        const std::lock_guard<std::mutex> lock(writer_mutex);
        ok = writer.write({recv_ns, venue, std::move(framed)});
      }
      (ok ? written : rejected).fetch_add(1);
    };
  };

  // Every venue is optional and independent; the recorder captures
  // whatever was asked for. Crypto needs no credentials and no registry
  // entry, which is what makes a cross-venue capture reproducible by
  // anyone who clones this.
  // Naming a venue on the command line means recording that venue, so the
  // registry-driven ones step aside rather than adding an unasked-for TLS
  // connection and unrelated traffic to the capture.
  std::unique_ptr<basis::feed::PolymarketFeed> poly_feed;
  if (!tokens.empty() && !crypto_only) {
    poly_feed = std::make_unique<basis::feed::PolymarketFeed>(
        basis::feed::PolymarketFeed::Config{.token_ids = tokens});
    poly_feed->set_raw_tap(make_tap(basis::model::Venue::Polymarket));
  }

  std::unique_ptr<basis::feed::BinanceFeed> binance_feed;
  if (!binance_symbols.empty()) {
    binance_feed = std::make_unique<basis::feed::BinanceFeed>(
        basis::feed::BinanceFeed::Config{.symbols = binance_symbols});
    binance_feed->set_raw_tap(make_tap(basis::model::Venue::Binance));
  }

  std::unique_ptr<basis::feed::CoinbaseFeed> coinbase_feed;
  if (!coinbase_products.empty()) {
    coinbase_feed = std::make_unique<basis::feed::CoinbaseFeed>(
        basis::feed::CoinbaseFeed::Config{.product_ids = coinbase_products});
    coinbase_feed->set_raw_tap(make_tap(basis::model::Venue::Coinbase));
  }

  // Kalshi requires an authenticated session even for market data; without
  // credentials the recording is Polymarket-only, stated up front rather
  // than discovered in the replay.
  std::unique_ptr<basis::feed::KalshiFeed> kalshi_feed;
  if (!kalshi_key_id.empty() && !crypto_only) {
    auto signer = basis::net::KalshiSigner::load(kalshi_pem, &error);
    if (!signer) {
      basis::log::error(error);
      return 1;
    }
    if (tickers.empty()) {
      basis::log::error("no kalshi tickers in " + config_path);
      return 1;
    }
    kalshi_feed = std::make_unique<basis::feed::KalshiFeed>(
        basis::feed::KalshiFeed::Config{.market_tickers = tickers,
                                        .key_id = kalshi_key_id,
                                        .signer = std::move(*signer)});
    kalshi_feed->set_raw_tap(make_tap(basis::model::Venue::Kalshi));
  }

  std::signal(SIGINT, handle_sigint);
  std::string what;
  const auto add_what = [&what](std::size_t n, const char* label) {
    if (n == 0) return;
    if (!what.empty()) what += " + ";
    what += std::to_string(n);
    what += ' ';
    what += label;
  };
  add_what(poly_feed ? tokens.size() : 0, "polymarket token(s)");
  add_what(kalshi_feed ? tickers.size() : 0, "kalshi ticker(s)");
  add_what(binance_symbols.size(), "binance symbol(s)");
  add_what(coinbase_products.size(), "coinbase product(s)");
  std::printf("recording %s -> %s%s\n", what.c_str(), out_path.c_str(),
              *seconds > 0 ? "" : "  (ctrl-c to stop)");
  // All venues are stamped by one process reading every socket, so the
  // streams share a clock. Two recorders, or two clocks, would put an
  // unknown offset straight into any cross-venue timing measured from the
  // capture (docs/bench/cross_venue_lead.md).
  if (poly_feed) poly_feed->start();
  if (kalshi_feed) kalshi_feed->start();
  if (binance_feed) binance_feed->start();
  if (coinbase_feed) coinbase_feed->start();

  const auto started = std::chrono::steady_clock::now();
  auto next_report = started + std::chrono::seconds(5);
  while (!g_interrupted.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto now = std::chrono::steady_clock::now();
    if (*seconds > 0 && now - started >= std::chrono::seconds(*seconds)) {
      break;
    }
    if (now >= next_report) {
      next_report += std::chrono::seconds(5);
      if (poly_feed) {
        std::printf("  poly %llu msgs %llu deltas %llu malformed %llu recon",
                    u(poly_feed->messages()),
                    u(poly_feed->deltas()),
                    u(poly_feed->malformed()),
                    u(poly_feed->reconnects()));
      }
      if (binance_feed) {
        std::printf("  |  binance %llu msgs %llu deltas %llu malformed",
                    u(binance_feed->messages()),
                    u(binance_feed->deltas()),
                    u(binance_feed->malformed()));
      }
      if (coinbase_feed) {
        std::printf("  |  coinbase %llu msgs %llu deltas %llu malformed",
                    u(coinbase_feed->messages()),
                    u(coinbase_feed->deltas()),
                    u(coinbase_feed->malformed()));
      }
      if (kalshi_feed) {
        std::printf("  |  kalshi %llu msgs %llu deltas %llu gaps %llu recon",
                    u(kalshi_feed->messages()),
                    u(kalshi_feed->deltas()),
                    u(kalshi_feed->gaps()),
                    u(
                        kalshi_feed->reconnects()));
      }
      std::printf("\n");
    }
  }
  if (poly_feed) poly_feed->stop();
  if (kalshi_feed) kalshi_feed->stop();
  if (binance_feed) binance_feed->stop();
  if (coinbase_feed) coinbase_feed->stop();

  std::printf("wrote %llu records to %s (%llu rejected)\n",
              u(written.load()),
              out_path.c_str(),
              u(rejected.load()));
  if (poly_feed) {
    std::printf("  polymarket: %llu malformed, %llu reconnects, "
                "%llu hashes verified, %llu mismatched\n",
                u(poly_feed->malformed()),
                u(poly_feed->reconnects()),
                u(poly_feed->hashes_verified()),
                u(poly_feed->hashes_mismatched()));
  }
  if (binance_feed) {
    std::printf("  binance: %llu malformed, %llu reconnects\n",
                u(binance_feed->malformed()),
                u(binance_feed->reconnects()));
  }
  if (coinbase_feed) {
    std::printf("  coinbase: %llu malformed, %llu reconnects, "
                "%llu levels unrepresentable\n",
                u(coinbase_feed->malformed()),
                u(coinbase_feed->reconnects()),
                u(coinbase_feed->unrepresentable()));
  }
  if (kalshi_feed) {
    std::printf("  kalshi: %llu malformed, %llu gaps, %llu reconnects\n",
                u(kalshi_feed->malformed()),
                u(kalshi_feed->gaps()),
                u(kalshi_feed->reconnects()));
  }
  std::printf("replay it:  basis replay %s --config %s\n", out_path.c_str(),
              config_path.c_str());
  return 0;
}

// The live pipeline's analytics half: everything downstream of the queue,
// owned by one analytics thread, with a mutex only for the periodic
// console snapshot taken by the main thread.
class LiveAnalytics {
 public:
  explicit LiveAnalytics(const basis::normalize::ContractRegistry& registry)
      : normalizer_(registry) {
    normalizer_.set_observer([this](const std::string& event_id,
                                    const basis::model::UnifiedBook& book,
                                    const basis::model::BookDelta& delta) {
      const auto kalshi_mid = book.mid(basis::model::Venue::Kalshi);
      const auto poly_mid = book.mid(basis::model::Venue::Polymarket);
      auto& ev = events_[event_id];
      ev.kalshi_mid = kalshi_mid;
      ev.poly_mid = poly_mid;
      if (kalshi_mid && poly_mid) {
        ev.divergence.observe(*kalshi_mid - *poly_mid);
        ev.lead_lag.observe(*kalshi_mid, *poly_mid, delta.ts_ns);
      }
    });
  }

  void on_delta(const basis::model::OwnedBookDelta& owned) {
    const std::lock_guard<std::mutex> lock(mutex_);
    ++deltas_;
    normalizer_.on_delta(owned.view());
  }

  void print_snapshot() {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::printf("-- %llu deltas, %llu unmapped\n",
                u(deltas_),
                u(
                    normalizer_.unmapped_deltas()));
    for (const auto& [event_id, ev] : events_) {
      if (ev.kalshi_mid && ev.poly_mid) {
        std::printf("   %-28s kalshi %5.1fc  poly %5.1fc  basis %+5.1fc\n",
                    event_id.c_str(), *ev.kalshi_mid, *ev.poly_mid,
                    *ev.kalshi_mid - *ev.poly_mid);
      } else if (ev.poly_mid) {
        std::printf("   %-28s poly %5.1fc  (no kalshi book yet)\n",
                    event_id.c_str(), *ev.poly_mid);
      } else if (ev.kalshi_mid) {
        std::printf("   %-28s kalshi %5.1fc  (no polymarket book yet)\n",
                    event_id.c_str(), *ev.kalshi_mid);
      }
    }
  }

  void print_final_report() {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [event_id, ev] : events_) {
      std::printf("\nevent %s\n", event_id.c_str());
      if (ev.divergence.samples() == 0) {
        std::printf("  basis    no overlap (one venue never had a "
                    "two-sided book)\n");
        continue;
      }
      std::printf("  basis    mean %+.2fc  min %+.2fc  max %+.2fc  "
                  "last %+.2fc  (%llu samples)\n",
                  ev.divergence.mean(), ev.divergence.min(),
                  ev.divergence.max(), ev.divergence.last(),
                  u(ev.divergence.samples()));
      const auto ll = ev.lead_lag.estimate();
      if (ll.correlation > 0.0) {
        const char* leader = ll.lead_seconds >= 0 ? "kalshi" : "polymarket";
        std::printf("  lead-lag %s leads by %.3fs  (corr %.2f over %llu "
                    "samples)\n",
                    leader,
                    ll.lead_seconds >= 0 ? ll.lead_seconds : -ll.lead_seconds,
                    ll.correlation,
                    u(ll.samples));
      }
    }
  }

 private:
  struct EventState {
    std::optional<double> kalshi_mid;
    std::optional<double> poly_mid;
    basis::analytics::DivergenceTracker divergence;
    basis::analytics::CrossCorrelationEstimator lead_lag;
  };

  std::mutex mutex_;
  basis::normalize::Normalizer normalizer_;
  std::map<std::string, EventState> events_;
  std::uint64_t deltas_ = 0;
};

int run_live(const std::vector<std::string_view>& args) {
  const auto config_path =
      flag_string(args, "--config", "configs/contracts.toml");
  const auto seconds = flag_value(args, "--seconds", 0);  // 0: until ctrl-c
  const auto report_every = flag_value(args, "--report", 5);
  if (!seconds || *seconds < 0 || !report_every || *report_every < 1) {
    basis::log::error("live: bad --seconds or --report value");
    return usage();
  }
  const auto kalshi_key_id = flag_string(args, "--kalshi-key-id", "");
  const auto kalshi_pem =
      flag_string(args, "--kalshi-pem", "secrets/kalshi.pem");

  std::string error;
  const auto registry =
      basis::normalize::TomlContractRegistry::load(config_path, &error);
  if (!registry) {
    basis::log::error(error);
    return 1;
  }

  // The one thread crossing in the engine: feed IO threads produce owned
  // deltas (a queued view into the parser buffer would dangle), one
  // analytics thread consumes. Bounded and blocking, so bursts back up
  // into TCP instead of dropping; the final report proves the accounting.
  basis::core::BoundedQueue<basis::model::OwnedBookDelta> queue(8192);
  const auto sink = [&queue](const basis::model::BookDelta& delta) {
    queue.push(basis::model::OwnedBookDelta(delta));
  };

  basis::feed::PolymarketFeed poly_feed(
      {.token_ids = registry->polymarket_tokens()});
  poly_feed.set_sink(sink);

  std::unique_ptr<basis::feed::KalshiFeed> kalshi_feed;
  if (!kalshi_key_id.empty()) {
    auto signer = basis::net::KalshiSigner::load(kalshi_pem, &error);
    if (!signer) {
      basis::log::error(error);
      return 1;
    }
    kalshi_feed = std::make_unique<basis::feed::KalshiFeed>(
        basis::feed::KalshiFeed::Config{
            .market_tickers = registry->kalshi_tickers(),
            .key_id = kalshi_key_id,
            .signer = std::move(*signer)});
    kalshi_feed->set_sink(sink);
  } else {
    std::printf("no kalshi credentials: streaming polymarket only\n");
  }

  LiveAnalytics analytics(*registry);
  std::thread analytics_thread([&] {
    while (auto owned = queue.pop()) {
      analytics.on_delta(*owned);
    }
  });

  std::signal(SIGINT, handle_sigint);
  poly_feed.start();
  if (kalshi_feed) kalshi_feed->start();

  const auto started = std::chrono::steady_clock::now();
  auto next_report = started + std::chrono::seconds(*report_every);
  while (!g_interrupted.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto now = std::chrono::steady_clock::now();
    if (*seconds > 0 && now - started >= std::chrono::seconds(*seconds)) {
      break;
    }
    if (now >= next_report) {
      next_report += std::chrono::seconds(*report_every);
      analytics.print_snapshot();
    }
  }

  poly_feed.stop();
  if (kalshi_feed) kalshi_feed->stop();
  queue.close();  // analytics drains what is queued, then exits
  analytics_thread.join();

  analytics.print_final_report();
  std::printf("\nqueue     %llu in, %llu out, high water %zu, "
              "%llu blocked pushes\n",
              u(queue.pushed()),
              u(queue.popped()),
              queue.high_water(),
              u(queue.blocked_pushes()));
  std::printf("feeds     poly %llu msgs %llu malformed %llu reconnects "
              "%llu hashes ok %llu mismatched",
              u(poly_feed.messages()),
              u(poly_feed.malformed()),
              u(poly_feed.reconnects()),
              u(poly_feed.hashes_verified()),
              u(
                  poly_feed.hashes_mismatched()));
  if (kalshi_feed) {
    std::printf("  |  kalshi %llu msgs %llu malformed %llu gaps "
                "%llu reconnects",
                u(kalshi_feed->messages()),
                u(kalshi_feed->malformed()),
                u(kalshi_feed->gaps()),
                u(kalshi_feed->reconnects()));
  }
  std::printf("\n");
  return 0;
}

}  // namespace basis::cli
