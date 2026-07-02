# basis

A real-time, cross-venue market-data engine in C++20. It ingests live order
books for the same real-world event from two structurally different prediction
markets, normalizes them into one schema, and measures the price *lead* of one
venue over the other.

- **Kalshi** is a CFTC-regulated, USD-denominated, centralized exchange.
- **Polymarket** runs on crypto rails (USDC): an off-chain order book with
  on-chain settlement on Polygon.

They list contracts on the same outcomes but have different participant bases,
capital efficiency, and settlement rails, so their prices diverge and one tends
to move first. `basis` measures that lead in real time, over an engine built
for low internal latency and zero message loss.

## Status

Early scaffold (Phase 0 of 6). The build, layering, test harness, and CI are in
place; the engine itself is being built phase by phase. See `PLAN.md` for the
full spec and the phase schedule.

## Build

```
cmake -B build -G Ninja
cmake --build build -j
./build/src/basis
ctest --test-dir build --output-on-failure
```

The Phase-0 configure pulls only GoogleTest. Live feeds, the BDE allocator
path, and the benchmark come online behind CMake options (`BASIS_ENABLE_NET`,
`BASIS_ENABLE_BDE`, `BASIS_ENABLE_BENCH`) as their phases land.

## Design at a glance

```
feed (Kalshi, Polymarket)  ->  normalize + match  ->  unified order book
                                                            |
                                                       analytics
                                                  (divergence, lead-lag)
                                                            |
                                          BLPAPI-style subscription API
```

The hot parse-and-normalize path is allocator-aware, built on Bloomberg's
open-source BDE (`bdlma`) arenas. The consumer interface mirrors Bloomberg's
BLPAPI subscription model. Internal ingest-to-signal latency is measured by
deterministic replay (network jitter removed) and reported in percentiles.

Note: this project uses Bloomberg's open-source libraries and API design. It
does not use Bloomberg data, which is licensed and not redistributable. The
only market data here comes from Kalshi's and Polymarket's public APIs.

## Layout

```
src/core/       logging, time, version
src/model/      canonical schema: venue, side, order book, deltas
src/net/        WebSocket client (Phase 1)
src/feed/       venue feed adapters: Kalshi, Polymarket (Phase 1/2)
src/normalize/  unified schema + cross-venue contract registry (Phase 3)
src/analytics/  divergence and lead-lag (Phase 5)
src/api/        BLPAPI-style subscription interface (Phase 6)
src/bench/      replay harness + latency histogram (Phase 4)
tests/          GoogleTest unit and property tests
configs/        contract registry
docs/bench/     committed captures and the numbers they produce
```

## Numbers

Filled in only from committed benchmarks, never aspirational (the same rule the
companion voxel-engine project follows). Until Phase 4/5 land, the headline
figures in `PLAN.md` are bracketed placeholders.
