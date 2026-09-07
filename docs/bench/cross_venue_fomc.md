# Cross-venue: the September 2026 FOMC decision, on Kalshi and Polymarket

The first capture in this repo where the two prediction-market venues quote
the *same* event, and the first that uses Kalshi's live feed at all: the
Kalshi adapter existed for months but had never connected, and when it did
its books came back empty (see the wire-format fix in `kalshi_parser.cpp`).

    gunzip -c docs/bench/fomc-xvenue.feedlog.gz > /tmp/fomc-xvenue.feedlog
    ./build/src/basis replay /tmp/fomc-xvenue.feedlog \
        --config configs/contracts-fomc.toml

    records   1510 (kalshi 218, polymarket 1292)
    session   1413.5 s span
    deltas    8598 applied, 0 unmapped
    dropped   0 malformed msgs, 0 bad lines, 0 gaps
    integrity 3 snapshot hashes verified, 0 mismatched

23.6 minutes, recorded 2026-09-06. Three outcomes of the same FOMC meeting:
hold (0 bps), hike 25 bps, cut 25 bps.

## The pairing is by hand, and that is the point

`configs/contracts-live.toml` leaves its `kalshi` fields empty on purpose:
matching an outcome across venues is a judgement about identical resolution
wording, and a script that guessed it would put a wrong pairing into every
number downstream. `configs/contracts-fomc.toml` fills them in by hand.
Each Kalshi outcome and its Polymarket question resolve on the same meeting
and both close 2026-09-16, checked against each venue's API.

The prices corroborate the pairing rather than merely permitting it: at
capture time Kalshi's hold contract quoted 0.48/0.49 against Polymarket's
0.495, and hike-25 quoted 0.49/0.50 against 0.495. Two venues independently
agreeing to within a cent is evidence the outcomes are the same one.

## An apparent arbitrage that fees delete entirely

On the hold contract, the raw books cross on **1,383 of 2,328** two-sided
updates - 59.4%, with a mean edge at the touch of **$1,083.49**. That is the
number a naive cross-venue scanner would print, and it is worthless:

    net of Kalshi taker fees   mean $-811.87   0/1383 crossed updates profitable
    fee-aware optimal sweep    mean $0.00      0/1383 worth taking
    surviving a 50ms delay     $0.00 (22/22 episodes)

Every single crossing loses money once Kalshi's taker fee is charged, and
the fee-aware sweep - which is free to take *part* of a crossed book if a
partial fill is profitable - declines all 1,383. The edge is smaller than
the cost of capturing it, at every depth and every reaction delay tested.

## The honest caveat, which is larger than the result

**59.7% of those samples were priced against a quote more than 5 seconds
old**, the stalest 60.8 seconds. At 218 Kalshi records in 23.6 minutes, the
venue simply does not update often enough for both books to be fresh at the
same instant. So an unknown share of the 59.4% "crossing" is a stale-quote
artifact rather than two live books genuinely disagreeing.

That does not change the conclusion - fees delete the edge whether or not
the crossing was real - but it does mean this capture cannot support a
claim about how often the venues *actually* cross. A capture that could
would need Kalshi quoting far more often than a Sunday afternoon provides.

Lead-lag is reported as no signal for the same reason: 525 Kalshi moves
produced 0 confirmed Polymarket follows, which at this update rate is an
absence of data rather than an absence of effect.

## What this capture is for

It pins three things that nothing else in `docs/bench/` does: that the
Kalshi feed works end to end against production, that a hand-paired
cross-venue registry produces a basis series, and that the fee model turns
a headline 59.4% arbitrage rate into zero taking opportunities.
