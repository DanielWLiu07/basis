#include <charconv>
#include <cstdio>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bench/replay_harness.h"
#include "bench/synth_generator.h"
#include "core/counting_resource.h"
#include "core/logger.h"
#include "core/version.h"
#include "normalize/contract_registry.h"

#ifdef BASIS_HAS_BDE
#include "alloc/bde_arena.h"
#endif

#ifdef BASIS_HAS_NET
#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

#include "feed/feed_log.h"
#include "feed/polymarket_feed.h"
#endif

namespace {

int usage() {
  std::printf(
      "basis %s - cross-venue prediction-market data engine\n"
      "\n"
      "usage:\n"
      "  basis synth <out.feedlog> [--steps N] [--lead-ms L] [--seed S]\n"
      "      generate a deterministic synthetic session (real wire formats,\n"
      "      injected cross-venue lead; ids match configs/synthetic.toml)\n"
      "\n"
      "  basis replay <in.feedlog> [--config <contracts.toml>]\n"
      "               [--alloc heap|count|bde]\n"
      "      replay a capture through parse -> normalize -> analytics -> api\n"
      "      and report basis, lead-lag, and ingest-to-signal latency\n"
      "      (default config: configs/synthetic.toml)\n"
      "      --alloc count reports heap traffic per message; --alloc bde\n"
      "      runs the hot path on Bloomberg bdlma arenas (needs a build\n"
      "      with BASIS_ENABLE_BDE)\n"
#ifdef BASIS_HAS_NET
      "\n"
      "  basis record <out.feedlog> [--config <contracts.toml>] [--seconds N]\n"
      "      capture live feeds for the configured contracts (Polymarket\n"
      "      needs no credentials); stop with --seconds or ctrl-c\n"
      "      (default config: configs/contracts.toml)\n"
#endif
      ,
      basis::kVersion);
  return 1;
}

std::optional<std::int64_t> parse_int(std::string_view s) {
  std::int64_t value = 0;
  const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
  if (ec != std::errc{} || ptr != s.data() + s.size()) return std::nullopt;
  return value;
}

// Flags are --name value pairs after the positional argument.
std::optional<std::int64_t> flag_value(const std::vector<std::string_view>& args,
                                       std::string_view name,
                                       std::int64_t fallback) {
  for (std::size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == name) {
      return parse_int(args[i + 1]);  // nullopt: malformed number
    }
  }
  return fallback;
}

std::string flag_string(const std::vector<std::string_view>& args,
                        std::string_view name, std::string fallback) {
  for (std::size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == name) return std::string(args[i + 1]);
  }
  return fallback;
}

int run_synth(const std::vector<std::string_view>& args) {
  if (args.empty()) return usage();
  const std::string out_path(args[0]);

  basis::bench::SynthConfig config;
  const auto steps = flag_value(args, "--steps", config.steps);
  const auto lead_ms = flag_value(args, "--lead-ms",
                                  config.lead_ns / 1'000'000);
  const auto seed = flag_value(args, "--seed",
                               static_cast<std::int64_t>(config.seed));
  // Bounds double as narrowing guards for the casts below.
  if (!steps || *steps <= 0 || *steps > 100'000'000 ||
      !lead_ms || *lead_ms < -86'400'000 || *lead_ms > 86'400'000 ||
      !seed || *seed < 0 || *seed > 4'294'967'295) {
    basis::log::error("synth: bad flag value");
    return usage();
  }
  config.steps = static_cast<int>(*steps);
  config.lead_ns = *lead_ms * 1'000'000;
  config.seed = static_cast<unsigned>(*seed);

  std::string error;
  if (!basis::bench::generate_synthetic_session(config, out_path, &error)) {
    basis::log::error(error);
    return 1;
  }
  std::printf("wrote %s (%d steps, %+lld ms injected kalshi lead, seed %u)\n",
              out_path.c_str(), config.steps,
              static_cast<long long>(config.lead_ns / 1'000'000), config.seed);
  std::printf("replay it:  basis replay %s\n", out_path.c_str());
  return 0;
}

void print_stats(const basis::bench::ReplayStats& stats) {
  std::printf("records   %llu (kalshi %llu, polymarket %llu)\n",
              static_cast<unsigned long long>(stats.records),
              static_cast<unsigned long long>(stats.kalshi_messages),
              static_cast<unsigned long long>(stats.polymarket_messages));
  std::printf("deltas    %llu applied, %llu unmapped\n",
              static_cast<unsigned long long>(stats.deltas),
              static_cast<unsigned long long>(stats.unmapped_deltas));
  std::printf("dropped   %llu malformed msgs, %llu bad lines, "
              "%llu gaps, %llu ignored\n",
              static_cast<unsigned long long>(stats.malformed),
              static_cast<unsigned long long>(stats.malformed_lines),
              static_cast<unsigned long long>(stats.gaps),
              static_cast<unsigned long long>(stats.ignored));

  const auto& lat = stats.latency;
  std::printf("latency   ingest-to-signal per record (us): "
              "p50 %.1f  p90 %.1f  p99 %.1f  max %.1f\n",
              static_cast<double>(lat.p50_ns) / 1e3,
              static_cast<double>(lat.p90_ns) / 1e3,
              static_cast<double>(lat.p99_ns) / 1e3,
              static_cast<double>(lat.max_ns) / 1e3);

  for (const auto& event : stats.events) {
    std::printf("\nevent %s\n", event.event_id.c_str());
    if (event.basis_samples == 0) {
      std::printf("  basis    no overlap (one venue never had a "
                  "two-sided book)\n");
      continue;
    }
    std::printf("  basis    mean %+.2fc  min %+.2fc  max %+.2fc  "
                "last %+.2fc  (%llu samples)\n",
                event.basis_mean, event.basis_min, event.basis_max,
                event.basis_last,
                static_cast<unsigned long long>(event.basis_samples));
    const auto& ll = event.lead_lag;
    if (ll.correlation <= 0.0) {
      std::printf("  lead-lag no signal (flat or too little overlap)\n");
    } else {
      const char* leader = ll.lead_seconds >= 0 ? "kalshi" : "polymarket";
      std::printf("  lead-lag %s leads by %.3fs  "
                  "(corr %.2f over %llu samples)\n",
                  leader,
                  ll.lead_seconds >= 0 ? ll.lead_seconds : -ll.lead_seconds,
                  ll.correlation,
                  static_cast<unsigned long long>(ll.samples));
    }
  }
}

// --alloc count: same heap allocator, with the traffic made visible.
class CountingParseArena final : public basis::bench::ParseArena {
 public:
  std::pmr::memory_resource* resource() override { return &counting_; }
  const basis::core::CountingResource& counts() const { return counting_; }

 private:
  basis::core::CountingResource counting_;
};

#ifdef BASIS_HAS_BDE
// --alloc bde: parse transients on a sequential arena dropped per message.
class BdeParseArena final : public basis::bench::ParseArena {
 public:
  std::pmr::memory_resource* resource() override { return arena_.resource(); }
  void release() override { arena_.release(); }

 private:
  basis::alloc::BdeSequentialArena arena_;
};
#endif

int run_replay(const std::vector<std::string_view>& args) {
  if (args.empty()) return usage();
  const std::string in_path(args[0]);
  const auto config_path =
      flag_string(args, "--config", "configs/synthetic.toml");
  const auto alloc_mode = flag_string(args, "--alloc", "heap");

  std::string error;
  const auto registry =
      basis::normalize::TomlContractRegistry::load(config_path, &error);
  if (!registry) {
    basis::log::error(error);
    return 1;
  }

  // Allocation setup outlives the harness: books free their nodes on
  // destruction, so their resource has to be destroyed after it.
  CountingParseArena counting_arena;
  basis::core::CountingResource counting_books;
#ifdef BASIS_HAS_BDE
  BdeParseArena bde_arena;
  basis::alloc::BdeMultipool bde_books;
#endif

  basis::bench::ParseArena* parse_arena = nullptr;
  std::pmr::memory_resource* book_mr = std::pmr::get_default_resource();
  if (alloc_mode == "count") {
    parse_arena = &counting_arena;
    book_mr = &counting_books;
  } else if (alloc_mode == "bde") {
#ifdef BASIS_HAS_BDE
    parse_arena = &bde_arena;
    book_mr = bde_books.resource();
#else
    basis::log::error("--alloc bde needs a build with BASIS_ENABLE_BDE=ON");
    return 1;
#endif
  } else if (alloc_mode != "heap") {
    basis::log::error("unknown --alloc mode: " + alloc_mode);
    return usage();
  }

  basis::api::InProcessSession session;
  basis::bench::ReplayHarness harness(*registry, &session, {}, book_mr);
  harness.set_parse_arena(parse_arena);
  const auto stats = harness.run(in_path, &error);
  if (!stats) {
    basis::log::error(error);
    return 1;
  }

  std::printf("replayed %s against %s (alloc %s)\n\n", in_path.c_str(),
              config_path.c_str(), alloc_mode.c_str());
  print_stats(*stats);

  std::printf("\npipeline  %.1f ms for %llu records (%.0fk records/sec)\n",
              static_cast<double>(stats->pipeline_ns) / 1e6,
              static_cast<unsigned long long>(stats->records),
              stats->pipeline_ns > 0
                  ? static_cast<double>(stats->records) * 1e6 /
                        static_cast<double>(stats->pipeline_ns)
                  : 0.0);
  if (alloc_mode == "count" && stats->records > 0) {
    const auto& parse = counting_arena.counts();
    const double n = static_cast<double>(stats->records);
    std::printf("allocs    parse %llu (%.1f/msg, %.0f B/msg), "
                "books %llu (%.1f/msg)\n",
                static_cast<unsigned long long>(parse.allocations()),
                static_cast<double>(parse.allocations()) / n,
                static_cast<double>(parse.bytes()) / n,
                static_cast<unsigned long long>(counting_books.allocations()),
                static_cast<double>(counting_books.allocations()) / n);
  }
  return 0;
}

#ifdef BASIS_HAS_NET

std::atomic<bool> g_interrupted{false};

void handle_sigint(int) { g_interrupted.store(true); }

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

  std::string error;
  const auto registry =
      basis::normalize::TomlContractRegistry::load(config_path, &error);
  if (!registry) {
    basis::log::error(error);
    return 1;
  }
  const auto& tokens = registry->polymarket_tokens();
  if (tokens.empty()) {
    basis::log::error("no polymarket tokens in " + config_path +
                      "; nothing to record");
    return 1;
  }

  basis::feed::FeedLogWriter writer(out_path);
  if (!writer.ok()) {
    basis::log::error("cannot open " + out_path + " for writing");
    return 1;
  }

  // The raw tap runs on the feed's IO thread and is its only writer user;
  // rejected writes are counted, per the no-silent-drop rule.
  std::atomic<std::uint64_t> written{0};
  std::atomic<std::uint64_t> rejected{0};
  basis::feed::PolymarketFeed feed({.token_ids = tokens});
  feed.set_raw_tap([&](std::string_view payload, std::int64_t recv_ns) {
    std::string framed(payload);
    // Some messages arrive with embedded newlines; outside JSON strings
    // whitespace is insignificant, so flattening keeps the payload valid
    // instead of losing a real message to the line framing.
    for (char& c : framed) {
      if (c == '\n' || c == '\r') c = ' ';
    }
    const bool ok = writer.write(
        {recv_ns, basis::model::Venue::Polymarket, std::move(framed)});
    (ok ? written : rejected).fetch_add(1);
  });

  std::signal(SIGINT, handle_sigint);
  std::printf("recording %zu polymarket token(s) -> %s%s\n", tokens.size(),
              out_path.c_str(),
              *seconds > 0 ? "" : "  (ctrl-c to stop)");
  feed.start();

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
      std::printf("  %llu msgs  %.1f KiB  %llu deltas  %llu malformed  "
                  "%llu reconnects\n",
                  static_cast<unsigned long long>(feed.messages()),
                  static_cast<double>(feed.bytes()) / 1024.0,
                  static_cast<unsigned long long>(feed.deltas()),
                  static_cast<unsigned long long>(feed.malformed()),
                  static_cast<unsigned long long>(feed.reconnects()));
    }
  }
  feed.stop();

  std::printf("wrote %llu records to %s (%llu rejected, %llu malformed, "
              "%llu reconnects)\n",
              static_cast<unsigned long long>(written.load()),
              out_path.c_str(),
              static_cast<unsigned long long>(rejected.load()),
              static_cast<unsigned long long>(feed.malformed()),
              static_cast<unsigned long long>(feed.reconnects()));
  std::printf("replay it:  basis replay %s --config %s\n", out_path.c_str(),
              config_path.c_str());
  return 0;
}

#endif  // BASIS_HAS_NET

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string_view> args(argv + 1, argv + argc);
  if (args.empty()) return usage();

  const auto command = args[0];
  const std::vector<std::string_view> rest(args.begin() + 1, args.end());
  if (command == "synth") return run_synth(rest);
  if (command == "replay") return run_replay(rest);
#ifdef BASIS_HAS_NET
  if (command == "record") return run_record(rest);
#endif
  return usage();
}
