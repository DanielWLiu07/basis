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

## The numbers (fill only from committed benchmarks)

Reconciled 2026-08-20 against `docs/bench/`. A figure lives here only once
a committed artifact produces it, and the wording has to survive someone
asking "measured how, on what?".

- **p99 37 us ingest-to-signal, p50 0.5 us**, on the committed 30 minute
  Polymarket capture (`docs/bench/latency.md`), 818-862k records/sec across
  five runs. The tail is structural - full-book snapshot messages - and
  stable run to run, so it is a property of the workload rather than the
  scheduler.
- **2.19M messages/sec, 8.2M deltas/sec** parsing and applying a live
  Binance capture against the venue's own 269 msgs/sec
  (`docs/bench/ingest.md`), best of five spanning 2.00-2.19M. A 300k floor
  is CI-gated on every commit (`scripts/perf_gate.sh`). This replaced an
  earlier 932k figure: the old number was measured against a book that was
  quietly accumulating phantom levels, and fixing the book made the walk
  cheaper. Do not quote the old one.
- **Allocator parity, not a win.** The zero-copy hot path runs at 1-2 heap
  allocations per message, verified by a counting `memory_resource` and
  pinned by a budget test, so Bloomberg `bdlma` arenas neither gain nor
  cost throughput (`docs/bench/allocator.md`). Heap stays the default; the
  measurement is the deliverable, and "we measured and chose the boring
  option" is the honest bullet.
- **Cross-venue lead: a direction, replicated - never a duration.** On two
  independent 45 minute Binance/Coinbase captures, repricings on Binance
  are answered by Coinbase far more often than the reverse, at every
  threshold from $0.25 to $2.00 (z = 11.76 on the first, 39.33 on a second
  session three times as busy). The follow *rates* do not replicate - both
  roughly doubled in the busier session - so the claim is the ordering and
  its significance. The cross-correlation estimator's interval includes
  zero, so there is no defensible number of milliseconds to quote, and the
  measurement is biased 36-86 ms *against* the result it reports
  (`docs/bench/cross_venue_lead.md`).

  This replaces the old `[N]-second cross-venue lead` bracket. That bracket
  was asking for a figure the data does not support, and filling it would
  have been the worst outcome available.
- **Matching engine: 23.5 ns/op, 42.6M ops/sec**, 2.1x a `std::map` book on
  the same order flow, with the two books' fills required to agree before
  the timings mean anything (`docs/bench/matching_engine.md`).
- **Zero message loss across 4 forced disconnects**, CI-gated
  (`tests/test_reconnect.cpp`): a fault-injecting local server hard-drops
  the TCP socket mid-subscription and the real feed stack rebuilds the book
  to ground truth every time, TLS peer and hostname verification on
  throughout.
- **Stall detection.** A dropped network does not always produce a read
  error: a half-open connection leaves a blocking read parked forever. One
  45 minute capture lost a venue for 25 minutes with zero reconnects
  logged. An idle watchdog now breaks a silent connection, `stalls()` is
  reported separately from `reconnects()`, and the second cross-venue
  capture ran with no gap over five seconds on either feed.

### What is measured, and what is not

The engine parses four venues. **Kalshi has never run live**, because it
requires an authenticated session even for market data and this repo has
never had an account; its adapter is verified offline down to the RSA-PSS
signature. Every cross-venue result here is therefore Binance/Coinbase,
the pairing that is public on both sides. Nothing in this repo may claim a
measured Kalshi/Polymarket lead until that key exists.

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

## Resume line (reconciled 2026-08-20)

> **Real-time cross-venue market-data engine (C++20).** Ingests live order
> books from four venues over TLS WebSocket into one normalized schema
> through a zero-copy hot path, proven at 1-2 heap allocations per message
> and at parity with Bloomberg BDE arenas by a counting-allocator
> benchmark, behind a BLPAPI-style subscription interface. **p99 37 us
> ingest-to-signal** on recorded live sessions and **2.19M messages/sec**
> parse-and-apply against a venue producing 269. Measured a **cross-venue
> price lead and replicated it** on an independent capture (two-proportion
> z = 11.8 and 39.3), under a measurement biased against the result.
> Zero message loss across forced disconnects and silent half-open
> connections, all of it reproducible from CI-gated benchmarks.

Notes on the wording, because each choice is load-bearing:

- "four venues" not "Kalshi and Polymarket": the previous line named the
  one venue that has never run live.
- "a cross-venue price lead and replicated it" not "an N-second lead": the
  data supports an ordering, not a duration.
- "at parity with BDE arenas" not "faster than": it was parity, and
  claiming otherwise is checkable in thirty seconds by anyone who opens
  `docs/bench/allocator.md`.
- "against a venue producing 269" is what makes 2.19M mean something. A
  throughput number with no denominator invites "so what?".
