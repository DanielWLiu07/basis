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

The offline engine runs end to end: real venue wire formats are parsed,
normalized into per-event unified books, and measured for basis, lead-lag,
and per-record ingest-to-signal latency, all driven by deterministic replay.
Live WebSocket adapters are the next phase; they slot in behind the same
`FeedAdapter` seam. See `PLAN.md` for the full spec and `docs/design.md` for
how the code is put together.

## Build and run

```
cmake -B build -G Ninja
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Try it without any credentials or network: generate a synthetic session with
a known injected cross-venue lead, then replay it through the full pipeline
and watch the engine report that lead back.

```
./build/src/basis synth captures/demo.feedlog --steps 5000 --lead-ms 400
./build/src/basis replay captures/demo.feedlog
```

The replay prints message accounting (nothing is ever silently dropped),
per-event basis statistics, the recovered lead, and ingest-to-signal latency
percentiles. The same closed loop runs in the test suite: if the engine
cannot recover an injected lead through the real parsers, the build is red.

The configure pulls GoogleTest and simdjson. Live feeds and the BDE
allocator path come online behind CMake options (`BASIS_ENABLE_NET`,
`BASIS_ENABLE_BDE`) as their phases land.

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
src/core/       logging, clocks, version
src/model/      canonical schema: venue, side, order book, unified book
src/feed/       venue parsers (Kalshi, Polymarket) + feedlog capture format
src/normalize/  cross-venue contract registry + event router
src/analytics/  divergence and cross-correlation lead-lag
src/api/        BLPAPI-style subscription interface
src/bench/      replay harness, latency recorder, synthetic sessions
src/net/        WebSocket client (next phase, live feeds)
tests/          GoogleTest unit and integration tests
configs/        contract registries (real + synthetic)
docs/           design notes, venue API notes, benchmark artifacts
```

## Numbers

Filled in only from committed benchmarks, never aspirational (the same rule the
companion voxel-engine project follows). Until Phase 4/5 land, the headline
figures in `PLAN.md` are bracketed placeholders.
