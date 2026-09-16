# Reliability: the failures this is built against


The table at the top links each measurement to its writeup. What follows is
the material those rows cannot carry: the failures the engine is built
against, and the checks that exist because a plausible number is more
dangerous than an error.

The failure this exists for is not a socket that errors. It is one that
does not: a peer or a middlebox that vanishes without a FIN or an RST
leaves a blocking read parked forever, and the reconnect path is driven by
read errors, so it never runs. That is not hypothetical here - a 45 minute
capture has Coinbase going silent 19 minutes in and staying silent for the
remaining 25, with **zero reconnects logged**, while Binance recovered
twice over the same outage because its server does close connections.
Beast's own `stream_base::timeout` does not cover it: those settings apply
to asynchronous operations and this client reads synchronously, and
`keep_alive_pings` has no effect while `idle_timeout` is `none`, which is
the client-role default. So the guard is external - a watchdog thread that
breaks the read the same way `stop()` does.

- `tests/test_reconnect.cpp` runs in CI on every commit: 4 forced
  mid-subscription TCP drops against a fault-injecting local server, and
  the feed stack must rebuild the book to ground truth with every drop
  counted, TLS peer and hostname verification on throughout.
- `fuzz/` runs in CI on every commit: libFuzzer on both venue parsers and
  the registry parser under ASan and UBSan, 100k executions per target
  per commit, 2M per target in local deep runs, zero findings in project
  code to date.
- `scripts/levelize.py` runs in CI before the build: a physical-design
  check over the `src/` include graph in the Lakos sense, failing on any
  component cycle, package cycle, or package dependency not declared to
  match what each library links. It caught a real one on its first run,
  where a single directory spanned two link-time tiers
  (`docs/design.md`).
- `docs/bench/fanout.md`: the consumer side of distribution. A synchronous
  session runs handlers on the publisher's thread, so the slowest consumer
  sets the publisher's rate: with one 50-microsecond handler the publisher
  is pinned near 20,000 updates/sec at every fan-out size, which is exactly
  1 / 50 microseconds. `api::ConflatingSession` gives each subscriber one
  slot per topic instead of a queue, so publishers never wait on consumers
  and memory is bounded by subscribers times topics rather than by publish
  rate. At 16 subscribers the publisher runs roughly 40 to 46x faster (777k to
  899k updates/sec across runs; it is a throughput, so it moves with load),
  and every subscriber including the slow one ends holding the current
  value: conflation drops the stale middle, never the present. Joining is
  snapshot-then-stream: a consumer connecting mid-session is seeded with
  the current image under the same lock the publisher fans out beneath, so
  it can neither miss an update nor receive one older than a value already
  waiting for it.
- Entitlements are enforced on that same session, default-deny in
  `Restricted` mode, and the interesting half is revocation: an
  entitlement that lapses mid-session must drop the value already waiting
  in the subscriber's slot AND withhold one published in the race after
  the revoke, so the check runs both at `revoke()` and again at delivery
  (`docs/bench/fanout.md`).
- `docs/bench/matching_engine.md`: the price-time-priority book sustains
  42.6M operations/sec at 23.5 ns/op on an M4, 2.1x a textbook std::map
  book replaying identical order flow, with per-class tail latency
  reported honestly against the clock's 41.7 ns tick. Measuring that tail
  found the resting path's 320 us worst case in container growth;
  pre-sizing cut it to 34.6 us with identical medians. A passive-fill
  study over the same flow separates fills from never-fills by queue
  position at placement: 3,600 contracts ahead versus 126,234, a 35x
  split, with 80.4% of resting orders eventually trading.

More of this kind of detail, on what the analytics compute and the failures
that shaped them: [`docs/analytics_notes.md`](analytics_notes.md).
