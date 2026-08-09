# Reconstructing a book, and proving it is the venue's book

Consuming a market-data API is not the hard part. The hard part is that a
venue hands you a stream of diffs and a separate snapshot, and whether the
book you assemble from them is the book the venue actually has is entirely
your problem. Get it wrong and nothing crashes: you get a book that looks
completely normal and is wrong, and every number downstream inherits the
error with no symptom.

`feed::BookSequencer` is the state machine for that, and
`basis book-verify` is the evidence that it works.

## The protocol

Binance's, and representative of the shape. Each depth event covers update
ids `[U, u]`; a REST snapshot carries a `lastUpdateId`. Reconstruction is
correct only if all of this holds:

- Events arriving before the snapshot is in hand are **buffered**, because
  the snapshot may turn out to predate them.
- Events wholly at or before the snapshot (`u <= lastUpdateId`) are
  **discarded**: the snapshot already contains them, and reapplying them
  would double-count.
- The first event applied must **straddle** the snapshot:
  `U <= lastUpdateId + 1 <= u`. If no buffered event does, the snapshot is
  older than the stream and cannot be joined from; the only correct move
  is to fetch a newer one.
- Afterwards every event must continue exactly: `U == previous u + 1`. One
  missing event means the book has silently diverged, and it cannot be
  repaired from the stream, only rebuilt.

The sequencer decides and does not apply. It takes update ids and returns
Buffer, Apply, Discard or Gap, so the rules can be tested exhaustively
without an order book, and the same logic serves any venue with this
protocol shape.

## The proof

A claim that reconstruction "works" is worth nothing unless it can fail.
`docs/bench/btcusdt-recon.feedlog.gz` is a live capture built to make it
falsifiable: BTCUSDT depth diffs, a REST snapshot taken two seconds in to
join from, and a second REST snapshot taken later, with the diff stream
continuing past it so the reconstruction can be driven to exactly that
snapshot's sequence point.

    ./build/src/basis book-verify docs/bench/btcusdt-recon.feedlog --levels 20

    applied=230 discarded=2 buffered=20 gaps=0 stale_snapshots=0 reached_target=1
    levels_compared=40 mismatches=0 exact

Twenty buffered before the snapshot arrived, two discarded as already
contained in it, 230 applied in unbroken sequence, and the resulting book
matches the venue's independently produced snapshot on all twenty levels
of both sides, at the same sequence id. Not "looks right": identical.

The comparison is only meaningful because the run reaches the validation
snapshot's `lastUpdateId` exactly (`reached_target=1`). An earlier version
of this experiment stopped eleven updates short and still matched, which
would have been luck rather than evidence; the capture was redone so the
stream runs past the validation point.

## What it does not cover

One symbol, one session, forty levels. Deeper levels are not compared
because the joining snapshot is limited to 1,000 levels per side and an
update below that horizon cannot be checked against it. Nothing here
exercises a real venue gap either: gaps are covered by unit tests that
construct them directly, because a live capture that happens to contain
one cannot be relied on to.
