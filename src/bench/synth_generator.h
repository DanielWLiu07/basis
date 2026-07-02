#pragma once

#include <cstdint>
#include <string>

namespace basis::bench {

// Generates a deterministic synthetic session in the venues' real wire
// formats: a latent probability random walk quoted by a synthetic Kalshi
// immediately and by a synthetic Polymarket lead_ns later. Because the lead
// is injected, replaying the file must report it back; that closed loop
// validates the whole engine with no network and no credentials.
//
// The identifiers default to the ones configs/synthetic.toml maps.
// Synthetic output validates the engine; its numbers are never quotable
// results (docs/bench rule).
struct SynthConfig {
  std::string kalshi_ticker = "SYN-DEMO";
  std::string polymarket_token = "1000001";
  int steps = 5000;                        // latent ticks
  std::int64_t step_ns = 100'000'000;      // latent tick interval: 100 ms
  std::int64_t lead_ns = 400'000'000;      // injected lead: Kalshi first
  unsigned seed = 42;
  // Fixed start (2026-01-01T00:00:00Z): same seed, same file, byte for byte.
  std::int64_t start_ts_ns = 1'767'225'600'000'000'000;
};

// Writes the session to path (feedlog format). False on failure, with the
// reason in *error.
bool generate_synthetic_session(const SynthConfig& config,
                                const std::string& path,
                                std::string* error = nullptr);

}  // namespace basis::bench
