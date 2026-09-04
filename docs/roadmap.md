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

### 1. Kalshi has never run live

The blocker is credentials, not code. The adapter exists and is verified
offline down to the RSA-PSS signature. `configs/contracts.toml` maps 14
cross-venue contracts. None of it has ever been exercised against the
venue, so every cross-venue result in this repo is Binance/Coinbase - the
substitute pairing, chosen because it is public on both sides.

The cost is a free account and an RSA key. It is the only item on this
list that cannot be done by writing code, and it is the one that unlocks
the most: with it, the fee-aware arbitrage backtester runs on a real
both-venue capture instead of a synthetic session, and the repo's stated
thesis becomes a measured result rather than a described one.

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
