# What is left here

An audit on 2026-09-03, run the same way as the sibling repo's: measure
first, then decide.

## Where this repo is strong

Test depth, and it is not close:

    basis   5,357 test lines over 10,828 source   ratio 0.49
    voxel   1,993 test lines over  9,313 source   ratio 0.21

Every core library - feed, model, analytics, api, exec, normalize, core -
has tests. The components with none are `cli/` command wiring and
`logger.cpp`, and CLI commands are mostly orchestration: open a file, call
a library, print. There is real value in testing them, and it is a long
way below the value of the gaps named next.

Argument parsing was the one place the CLI gap had teeth, and it has been
closed: `flag_double` used to substitute a fallback for unparseable input,
so `--speed banana` silently replayed unpaced.

## The three real gaps, ranked

### 1. ~~Kalshi has never run live~~ CLOSED

This said the adapter was verified offline and had never been exercised
against the venue, so every cross-venue result was Binance/Coinbase.

It is done. A key arrived, the handshake worked first time, and the books
came back empty - Kalshi had migrated to decimal dollar strings under new
field names and the parser read the missing fields as a legal empty book.
Every snapshot parsed, every book was empty, and the malformed counter
read zero. Fixed in #69, and `docs/bench/fomc-xvenue.feedlog.gz` is the
committed capture that came out of it: 218 Kalshi records quoting the same
event as Polymarket. The failure is written up in postmortems.md.

Leaving the heading struck through rather than deleting it: this entry is
why the capture exists, and a roadmap that only ever shows what is left
loses the record of what a gap cost to close.

### 2. This is a book engine, not a market-data engine

`BookDelta` carries price levels. `Action` is Set, Add, Clear. There are
no trade prints, no last-trade, no volume - and every venue here publishes
them on the same socket.

The Coinbase parser can already read a `ticker` frame, which carries
trades. The live feed subscribes `level2_batch` instead, for depth, so no
committed capture contains a single trade. That is what makes this bigger
than it looks: it needs a channel change and a fresh capture, not a parser
change.

Worth it because it is half of what a market-data feed carries, and
because it would give `ConflatingSession` its counter-example. That doc
already argues fills and prints want a queue rather than conflation, and
there is nothing in the repo to point at.

### 3. There is no storage layer

`.feedlog` is one record per line, tab-separated, raw JSON, gzipped. That
is a capture format. There is no index, no columnar layout, no time-range
query; reading a thirty-minute window means scanning the file.

This is the largest piece of work on the list and the one furthest from
what exists. It is also the only one that would add a capability the repo
does not gesture at anywhere else.

## Closed since this was written

**The request half of BLPAPI.** The consumer interface had subscription
and not request; it now has both, entitlement-checked identically, with a
refusal deliberately indistinguishable from a topic that does not exist.
That was listed as the highest-leverage addition for a market-data reader
and it took a day.

**Whether a short capture can stand in for a long one.**
`docs/bench/extrapolation.md`. The median can, the tail cannot, and the
gap is a factor of 63 - which retroactively justifies every percentile in
this directory coming from the thirty-minute and four-hour captures.

## What is explicitly not on the list

**Kafka, for replay or event streaming.** Considered and rejected on
numbers. Kafka is a milliseconds tool and this engine's headline is a
microsecond service time; putting it in the path deletes the number the
repo is built on, and putting it outside the path leaves it doing nothing.
`kafka market data` is also 1,170 GitHub repositories, which is the
default tutorial architecture rather than a differentiator. The gap it
would fill - cross-process fan-out - is real, and the domain-correct
answer to it is a shared-memory ring buffer, which is what CME and
Chronicle actually use and which preserves the latency story instead of
destroying it.

**More estimators.** Three already agree on the cross-venue ordering:
cross-correlation, an event study, and Hayashi-Yoshida. A fourth would not
make the finding more true.

**Chasing throughput.** ~2M messages/sec against a venue producing 269.
The ratio is the point and it is already four orders of magnitude.
