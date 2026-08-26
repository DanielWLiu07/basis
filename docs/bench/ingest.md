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

    records=24048 ok=24048 ignored=0 malformed=0 deltas=90411
    venue_span_s=89.2   venue_msgs_per_sec=269
    engine_ms=11.0      engine_msgs_per_sec=2189981
                        engine_deltas_per_sec=8233465   headroom=8128x

Best of five; the five runs spanned 2.00M to 2.19M messages/sec on a
machine at load 4.4, median 2.15M.

Read the two rates against each other. The venue, on a busy crypto book
across 94 symbols, sustains 269 messages a second. The same pipeline
parses and applies those messages at 2.19 million a second. The engine is
not the bottleneck by four orders of magnitude, and that is the honest
reading of every latency number in `docs/bench/latency.md`: they describe
an engine with enormous headroom over the venues it consumes, not an
engine pushed to its limit.

`ok=24048, malformed=0` is the other half of the result. Every message the
venue sent parsed, across 73,302 prices, which is the coverage claim a
parser written against documentation rather than data cannot make.

These figures replace an earlier 932,471 messages/sec at 3,461x headroom,
and the reason is a correctness fix rather than an optimization. A
`bookTicker` message replaces the touch, but the parser emitted it as two
`Set` deltas without clearing the previous best bid and ask, so the book
accumulated stale levels and grew without bound - hundreds of levels deep
on this capture, with a crossed spread. Every apply was therefore doing
more work on a book that was also wrong. Clearing the touch first (see
`docs/bench/cross_venue_lead.md`, where the bug surfaced) both fixes the
book and cuts the work, which is why the delta count rose to 90,411 while
the time fell. A throughput benchmark cannot catch a wrong book; it took
a measurement that actually read prices out of one.

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

## Every committed capture, and the defect that hid two of them

`ingest-bench` takes any capture in the repo, because each record is parsed
by the parser its venue column names:

    binance-90s      records=24048  ok=24048  deltas=90411   269 msg/s  headroom=4578x
    live-poly-30min  records=34731  ok=34273  deltas=266597   19 msg/s  headroom=82832x
    btc-xvenue-2     binance + coinbase in one file, both dispatched

Read the middle row against the first. The Polymarket capture reports a
far larger headroom than the Binance one and that is not the engine being
faster: it is the venue being slower. 19 messages a second against 269 is
the whole reason `binance-90s` exists.

Until this was fixed the command hardcoded the Binance parser and ignored
the venue column. On the Polymarket capture that produced

    INGEST records=34731 ok=0 ignored=34731 deltas=0
    INGEST engine_msgs_per_sec=2426733 headroom=125717x

which is a timed loop measuring how fast simdjson rejects a message it was
never given, printed as ingest throughput. Nothing published here used the
wrong input, so no figure in this file was ever affected, but the failure
mode is worth naming: the output looked normal, the number looked good,
and the only tell was an `ok=0` two lines above it. A run that produces no
deltas is now an error rather than a result.

## What this does not claim

These are parse and book-apply numbers on a recorded capture, with file IO
excluded by loading the records first. They are not end-to-end network
latency, and the Binance path has no sequence-gap reconciliation: the
venue's `depthUpdate` carries first/last update ids for replay against a
REST snapshot, which a live adapter would need and the offline benchmark
does not do.
