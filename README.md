# basis

A cross-venue market-data engine in C++20. It reads live order books from
four venues over TLS WebSocket, normalizes them into one schema, and serves
them to consumers through a BLPAPI-style interface: subscription and
request, both entitlement-checked.

Then it measures which venue's price moves first.

## Numbers

Every row traces to one command on a capture committed to this repo. No
figure here is aspirational; if something is not yet measured, it says so.

| Measured | Result | Where |
|---|---|---|
| Cross-venue price discovery | Binance repricings answered 57.7% of the time, Coinbase 26.4% (z = 11.76, 204,864 messages) | [`cross_venue_lead.md`](docs/bench/cross_venue_lead.md) |
| Ingest throughput | 2.15M messages/sec median of five runs (2.00 to 2.19M), 8.2M book deltas/sec | [`ingest.md`](docs/bench/ingest.md) |
| Ingest-to-signal latency | p50 0.8 us, p99 78 us at 458k records/sec | [`latency.md`](docs/bench/latency.md) |
| Coordinated omission | slowest 1% of records wait 13.3x their own processing time (116 us against 1,534 us) | [`latency.md`](docs/bench/latency.md) |
| Conflated fan-out | publisher runs 40 to 46x faster than inline fan-out at 16 subscribers | [`fanout.md`](docs/bench/fanout.md) |
| Hot path allocations | 1 to 2 heap allocations per message, BDE arenas at parity | [`allocator.md`](docs/bench/allocator.md) |
| Unattended soak | 4 hours live, 59,891 messages, 3 venue disconnects recovered, zero gaps | [`soak.md`](docs/bench/soak.md) |
| Cross-venue arbitrage | books cross on 59.4% of updates, 0 of 1,383 profitable after fees | [`cross_venue_fomc.md`](docs/bench/cross_venue_fomc.md) |
| Book reconstruction | 40 of 40 levels match the venue's own REST snapshot, zero mismatches | [`book_reconstruction.md`](docs/bench/book_reconstruction.md) |
| Matching engine | 42.6M operations/sec at 23.5 ns/op, 2.1x a `std::map` book | [`matching_engine.md`](docs/bench/matching_engine.md) |

`scripts/bench.sh` regenerates these from the committed captures through
`replay --json`. `scripts/perf_gate.sh` runs in CI on every commit and
fails the build if lead recovery, integrity counters, the allocation
budget, or the throughput floor regress.

## Try it in sixty seconds

No network, no credentials. Generate a synthetic session with a known
cross-venue lead injected, then replay it through the real parsers and
watch the engine report that lead back.

```
cmake -B build -G Ninja
cmake --build build -j
./build/src/basis synth captures/demo.feedlog --steps 5000 --lead-ms 400
./build/src/basis replay captures/demo.feedlog
```

Replay prints message accounting (nothing is ever silently dropped),
per-event basis, the recovered lead with a bootstrap confidence interval
and an independent event-study cross-check, and latency percentiles. The
same closed loop runs in CI: if the engine cannot recover an injected lead
through the real parsers, by both methods, the build is red.

![Synthetic session with the injected 400 ms cross-venue lead visible](docs/img/synth-lead.png)

The injected lead is visible in the session itself: one random walk quoted
by two synthetic venues, one of them 400 ms behind. Replay reports 0.400 s
at correlation 1.00.

## How it fits together

```
  Kalshi ----+
  Polymarket +
  Binance ---+---> feed handlers ---> normalize ---> unified book
  Coinbase --+     (4 parsers,        (venue ids        (per event,
                   one adapter         become one        both venues)
                   seam)               event id)              |
                                                              v
                                                         analytics
                                          (basis, lead-lag, microprice,
                                           crossed-book economics net of
                                           fees and of reaction delay)
                                                              |
                                                              v
                                       BLPAPI-style API: subscription (push,
                                       conflated) + request (pull), both
                                       entitlement-checked, default-deny

  exec/ ---> price-time-priority matching engine, Gtc/Ioc/Fok
             re-executes the analytics' sweeps as order flow so the
             two must agree
```

Four things happen here, and the middle two are where a market-data team
spends its time.

**Feed handlers.** Four venue parsers behind one adapter seam, each turning
a venue's dialect into one canonical delta. Sequence gaps are detected and
answered by dropping the book and re-requesting a snapshot, because a book
known to be stale must never be served.

**Normalization.** Venue-native market ids disappear at the registry;
everything downstream is keyed by a venue-neutral event id.

**Distribution.** Conflated fan-out, so a slow consumer receives the current
price rather than a backlog, with memory bounded by subscribers times topics
rather than by publish rate.

**Entitlements.** Default-deny, granted per subscriber and per topic,
enforced identically on push and pull, with denials counted rather than
merely prevented.

## The headline result

**A repricing on Binance is answered by Coinbase far more often than the
reverse, and it holds on two instruments at once.**

| | answered first | |
| :--- | ---: | :--- |
| Binance leads | **57.7%** | of Coinbase repricings |
| Coinbase leads | 26.4% | of Binance repricings |

z = 11.76 over 204,864 messages from a 29-minute capture, with three
independent estimators agreeing on the ordering: cross-correlation, an
event study, and Hayashi-Yoshida. The capture is committed, so the result
regenerates offline:

```
./build/src/basis replay docs/bench/btc-eth-xvenue.feedlog.gz --json
```

Method, controls, and what would falsify it:
[`docs/bench/cross_venue_lead.md`](docs/bench/cross_venue_lead.md).

## Build options

```
ctest --test-dir build --output-on-failure
```

The configure pulls GoogleTest and simdjson. With `-DBASIS_ENABLE_BDE=ON`
(`brew install bde` on macOS), `replay --alloc bde` runs the hot path on
Bloomberg `bdlma` arenas and `--alloc count` reports heap traffic per
message. `replay --breakdown` splits ingest-to-signal time into parse
versus everything downstream: on the synthetic session parse is about 60%
(simdjson plus canonicalization), the rest of the pipeline the other 40%.

## Read more

Each row in the table above links to the measurement that produced it.
These carry the parts a table cannot:

**Results and methods**
- [`docs/bench/`](docs/bench/) - one writeup per measurement, each with
  the command that regenerates it from a committed capture
- [`docs/analytics_notes.md`](docs/analytics_notes.md) - what the
  analytics compute, and the failures that shaped them

**How it is built**
- [`docs/design.md`](docs/design.md) - the pipeline, module by module,
  plus the matching engine and the source layout
- [`docs/venues.md`](docs/venues.md) - why this venue pairing, and which
  venues have actually streamed rather than been implemented
- [`PLAN.md`](PLAN.md) - the full spec

**Why it survives contact with a real feed**
- [`docs/reliability.md`](docs/reliability.md) - the socket failure that
  does not error, the reconnect tests, and the fuzzers
- [`docs/postmortems.md`](docs/postmortems.md) - five defects that
  survived their own test suites, and why nothing caught them
- [`docs/live_capture.md`](docs/live_capture.md) - recording your own,
  and the registry rot that makes an empty capture ambiguous
