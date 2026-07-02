#include <charconv>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bench/replay_harness.h"
#include "bench/synth_generator.h"
#include "core/logger.h"
#include "core/version.h"
#include "normalize/contract_registry.h"

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
      "      replay a capture through parse -> normalize -> analytics -> api\n"
      "      and report basis, lead-lag, and ingest-to-signal latency\n"
      "      (default config: configs/synthetic.toml)\n",
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
  if (!steps || !lead_ms || !seed || *steps <= 0) {
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

int run_replay(const std::vector<std::string_view>& args) {
  if (args.empty()) return usage();
  const std::string in_path(args[0]);
  const auto config_path =
      flag_string(args, "--config", "configs/synthetic.toml");

  std::string error;
  const auto registry =
      basis::normalize::TomlContractRegistry::load(config_path, &error);
  if (!registry) {
    basis::log::error(error);
    return 1;
  }

  basis::api::InProcessSession session;
  basis::bench::ReplayHarness harness(*registry, &session);
  const auto stats = harness.run(in_path, &error);
  if (!stats) {
    basis::log::error(error);
    return 1;
  }

  std::printf("replayed %s against %s\n\n", in_path.c_str(),
              config_path.c_str());
  print_stats(*stats);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string_view> args(argv + 1, argv + argc);
  if (args.empty()) return usage();

  const auto command = args[0];
  const std::vector<std::string_view> rest(args.begin() + 1, args.end());
  if (command == "synth") return run_synth(rest);
  if (command == "replay") return run_replay(rest);
  return usage();
}
