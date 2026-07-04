# basis - build plan

A real-time, cross-venue market-data engine in C++20. It ingests live order
books for the *same real-world event* from two structurally different venues,
normalizes them into one schema, and measures the price *lead* of one venue
over the other.

This file is the living spec. It is the source of truth for scope and for the
numbers the README/resume are allowed to claim. Keep it honest: a number lives
here only once a committed benchmark produces it.

## Thesis

Kalshi and Polymarket list contracts on the same real-world outcomes but are
structurally different markets:

- **Kalshi** - CFTC-regulated, USD-denominated, centralized matching.
- **Polymarket** - crypto rails (USDC), an off-chain CLOB with **on-chain
  settlement on Polygon**.

(Accuracy note: Polymarket is *not* an on-chain order book. Matching is
off-chain; only settlement/custody is on-chain. Both venues are CLOBs at the
matching layer. The honest contrast is regulated-USD-centralized vs.
crypto-rails-USDC-on-chain-settlement - never claim "on-chain matching".)

Because the two venues have different participant bases, capital efficiency,
and settlement rails, their prices for the same event diverge, and one tends to
*lead* the other. The project measures that lead.

## What makes it different (the hook)

Same event, two incompatible market structures, and a **measured lead-lag** in
real time. Most market-data side projects display prices; this one *discovers
and quantifies a relationship* between two venues nobody instruments against
each other. The result is a falsifiable number, the way "18x meshing" is on the
voxel engine.

## What this is NOT (scope guard)

- Not a trading bot. No orders are ever placed. Read-only market data.
- Not a dashboard-first project. A view is stretch; the engine + the measured
  result are the deliverable.
- Not an HFT claim. Prediction markets move in seconds. The microsecond story
  is the **engine's internal ingest-to-signal latency**, measured by
  deterministic replay - never a claim about the market's tick rate.
- Not built on Bloomberg *data* (Terminal/BLPAPI feeds are paid/enterprise and
  license-restricted; public repo ships only Kalshi/Polymarket data, which is
  free and legal to display). We use Bloomberg's *open-source* tech (BDE) and
  *API design* (BLPAPI-style interface), not their data.

## Architecture (clean layering - do not violate)

```
src/
  core/       Logger, Time, Config, version          (deps: nothing)
  model/      Venue, Side, PriceLevel, OrderBook,     (deps: core)
              BookDelta, UnifiedBook                  canonical schema
  net/        WsClient (Beast+TLS), backoff           (deps: core)
  feed/       FeedAdapter (iface), KalshiFeed,        (deps: net, model, core)
              PolymarketFeed                          venue JSON -> canonical
  normalize/  Normalizer, ContractRegistry,           (deps: model, core)
              arena (BDE bdlma)                        unify + match events
  analytics/  Divergence (basis), LeadLag             (deps: model, core)
  api/        Subscription, Session (BLPAPI-style)    (deps: model, core)
  bench/      ReplayHarness, LatencyHistogram         (deps: all)
  main.cpp
tools/
  record.cpp  capture live feeds to disk (timestamped)
  replay.cpp  deterministic replay for the bench
tests/        unit + property tests (GoogleTest)
configs/
  contracts.toml   matched-event registry (Kalshi <-> Polymarket)
docs/bench/        committed captures + numbers
```

Rules:
- `core/` and `model/` know nothing about networking or venues.
- `feed/` is the only layer that knows venue-specific JSON; it emits canonical
  `model::BookDelta` only.
- `normalize/` owns the unified schema and the cross-venue contract mapping.
- `analytics/` consumes the unified book; it never touches sockets.
- GPU... n/a. Sockets run on IO threads; analytics on its own thread; the
  crossing points are explicit queues. Like the voxel engine: keep the
  thread-crossing boundary small and obvious.

## Tech stack

- C++20, CMake (3.20+), Ninja, deps via FetchContent where sane.
- **Boost.Beast + OpenSSL** - WSS feeds (Phase 1).
- **simdjson** - fast, low-alloc JSON parse (Phase 1).
- **Bloomberg BDE `bdlma`** - arena/sequential allocators in the hot path
  (Phase 3). Use *only* `bdlma`; do not adopt all of `bsl`/`bde`.
- **HdrHistogram (C++)** - latency percentiles (Phase 4).
- **GoogleTest** - unit/property tests (Phase 0+).
- **GitHub Actions** - build + test + TSan/ASan + perf-regression gate.

simdjson + bdlma arenas + lock-free queue + percentile latency is one coherent
microsecond-systems story.

## Dependency onboarding by phase

The default build pulls only GoogleTest and simdjson (the parsers are the
heart of the offline pipeline, so JSON parsing is a core dependency).
Heavier deps come online in the phase that first needs them, each behind a
CMake option:

| Dep            | Option                  | Phase |
|----------------|-------------------------|-------|
| GoogleTest     | BASIS_BUILD_TESTS (ON)  | 0     |
| simdjson       | always on               | 0     |
| Boost.Beast    | BASIS_ENABLE_NET (OFF)  | 1     |
| BDE bdlma      | BASIS_ENABLE_BDE (OFF)  | 3     |
| HdrHistogram   | BASIS_ENABLE_BENCH (OFF)| 4     |

## Build plan (~2.5-3 weeks)

| Phase | Days  | Deliverable                                                       | Number produced                        |
|-------|-------|-------------------------------------------------------------------|----------------------------------------|
| 0     | 1     | Repo, CMake, CI (build+test+sanitizers), README stub, model types | green CI                               |
| 1     | 2-3   | Kalshi feed: live book, snapshot+delta, reconnect, gap re-snapshot | -                                      |
| 2     | 4-5   | Polymarket feed: live book, same reliability                       | -                                      |
| 3     | 6-7   | Normalize + contract match; hot path through bdlma arena           | allocs/msg, arena vs heap (done: parity) |
| 4     | 8-9   | Record/replay harness + HdrHistogram instrumentation               | p99 ingest->signal (us), throughput     |
| 5     | 10-12 | Divergence (basis) + lead-lag estimator over recorded sessions     | N-second cross-venue lead (+ N samples)|
| 6     | 13-15 | BLPAPI-style subscription API; bench wired to CI gate; README      | zero loss across K reconnect/gap events|

Stretch (after MVP solid): more venues, fuzzy auto contract-matching, live TUI
view, Tracy capture.

## Reliability requirements (Bloomberg cares about this more than raw speed)

- Reconnect with exponential backoff + jitter on any socket drop.
- Sequence-gap detection on the delta stream; on gap, drop the local book and
  re-request a snapshot. Never serve a book known to be stale.
- Backpressure: bounded queues between IO and analytics; measure, never
  silently drop without counting.
- Every dropped/recovered message is *counted* and surfaced - "zero message
  loss across K events" is a measured claim, not a hope.

## Lead-lag methodology (so the finding is defensible)

1. Record synchronized, timestamped mid-price series for matched events.
2. Cross-correlate venue-A vs venue-B return series at a range of lags; the lag
   maximizing correlation is the lead.
3. Cross-check with an event study: when venue A's mid moves > threshold,
   measure time until B follows.
4. Report **median lead + sample size + confidence**. Call it observational;
   never claim causation.

## The numbers (resume brackets - fill only from committed benchmarks)

- p99 37 us ingest-to-signal, p50 0.5 us, filled 2026-07-04 from a
  committed 30-minute live capture (docs/bench/latency.md); the tail is
  structural (full-book snapshot messages), stable across runs
- 840k msgs/sec sustained on that live capture (3.7M records/sec on the
  smaller-message synthetic mix, docs/bench/allocator.md); a 300k floor
  is CI-gated on every commit (scripts/perf_gate.sh)
- allocator: measured parity, filled 2026-07-02 (docs/bench/allocator.md).
  The zero-copy hot path runs at 1 to 2 heap allocations per message,
  verified by a counting memory_resource and pinned by a budget test, so
  bdlma arenas neither gain nor cost throughput at 3.7M records/sec replay.
  Heap stays the default; the measurement is the deliverable.
- `[N]`-second cross-venue lead (with sample size)
- zero message loss across `[K]` reconnect/gap events

## Risks & mitigations

- **Event overlap**: the two venues won't list identical contracts everywhere.
  Target categories with real overlap (elections, Fed/macro, sports). The
  contract registry maps them; start config-driven, fuzzy-match later.
- **API auth/limits**: validated 2026-06 (see `docs/api_integration.md`).
  Kalshi requires an RSA-signed authenticated session even for the public
  `orderbook_delta` channel (free account + API key, loaded from a local-only
  file). Polymarket's `market` channel is public, no auth. Both expose live
  order-book snapshot+delta WebSockets, so the thesis is buildable.
- **BDE build weight**: scope to `bdlma` only; time-box the integration.
- **Slow-market critique**: pre-answered - microsecond cred is the engine's
  internal latency measured by replay, not the market tick rate.

## Resume line (final form, brackets filled from docs/bench/)

> Real-Time Cross-Venue Prediction-Market Data Engine - C++20 engine ingesting
> Kalshi and Polymarket order books through a zero-copy hot path, proven at
> 1-2 heap allocations per message and at parity with Bloomberg BDE arenas
> by a counting-allocator benchmark, with a BLPAPI-style subscription
> interface; p99 37 us ingest-to-signal at 840k msgs/sec on recorded live
> sessions, measured a [N]-second cross-venue price lead, zero message
> loss, reproducible from a CI-gated benchmark.
