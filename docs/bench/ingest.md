# Ingest throughput on a venue that produces load

The prediction markets this engine is built around are slow. The committed
30-minute Polymarket capture carries 34,731 messages over 1,799 seconds:
about 19 messages a second. That is a real property of the subject, not a
capture artifact, and it makes the latency numbers hard to interpret. An
engine measured only against 19 messages a second has not been measured.

So the engine also ingests Binance, which is here for exactly one reason:
it produces load. `docs/bench/binance-90s.feedlog.gz` is 90 seconds of
live public market data captured off the venue's combined stream, 94
symbols on `@bookTicker` plus 40 on `@depth@100ms`.

    ./build/src/basis ingest-bench docs/bench/binance-90s.feedlog

    records=24048 ok=24048 ignored=0 malformed=0 deltas=73302
    venue_span_s=89.2   venue_msgs_per_sec=269
    engine_ms=25.8      engine_msgs_per_sec=932471
                        engine_deltas_per_sec=2842315   headroom=3461x

Read the two rates against each other. The venue, on a busy crypto book
across 94 symbols, sustains 269 messages a second. The same pipeline
parses and applies those messages at 932,471 a second. The engine is not
the bottleneck by three and a half orders of magnitude, and that is the
honest reading of every latency number in `docs/bench/latency.md`: they
describe an engine with enormous headroom over the venues it consumes,
not an engine that has been pushed to its limit.

`ok=24048, malformed=0` is the other half of the result. Every message the
venue sent parsed, across 73,302 prices, which is the coverage claim a
parser written against documentation rather than data cannot make.

## The price-model decision

Prediction-market contracts settle at $0 or $1, so the engine's canonical
price is an integer cent, and the matching engine's flat 99-slot ladder
depends on it. Crypto does not share that constraint: `ALLOUSDT` quotes
`0.32430000`, which is 32.43 cents and not representable here.

The parser rejects such a message as malformed rather than truncating it,
because a truncated price is a wrong book handed to the analytics, and a
wrong book is worse than a missing one. The live subscription therefore
covers the 94 USDT symbols whose `PRICE_FILTER` tick is exactly 0.01, and
the capture above confirms the restriction empirically: 73,302 prices,
none off the cent grid.

That is a limitation, stated rather than hidden. Supporting the full
symbol universe means widening the canonical price from integer cents to a
scaled integer, which touches the order book, the analytics, and the
matching engine's ladder assumption. The capture is enough to measure
ingest, so the widening has not been done.

## What this does not claim

These are parse and book-apply numbers on a recorded capture, with file IO
excluded by loading the records first. They are not end-to-end network
latency, and the Binance path has no sequence-gap reconciliation: the
venue's `depthUpdate` carries first/last update ids for replay against a
REST snapshot, which a live adapter would need and the offline benchmark
does not do.
