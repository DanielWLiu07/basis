# Crossed-book economics: methodology and numbers

Every figure here regenerates from one command against the deterministic
synthetic session (fixed seed, so the input is byte-identical anywhere):

    ./build/src/basis synth /tmp/econ.feedlog --steps 5000 --lead-ms 400
    ./build/src/basis replay /tmp/econ.feedlog          # human report
    ./build/src/basis replay /tmp/econ.feedlog --json   # same fields, machine
    ./build/src/basis replay /tmp/econ.feedlog --episodes-csv /tmp/eps.csv

The ladder, rung by rung. Each answers the question the previous one
raises, and each is a separate, testable computation:

1. Frequency: crossable updates / two-sided updates. One venue's best bid
   above the other's best ask is an actual fees-aside arbitrage, unlike a
   mid-price gap that never clears the spreads.
2. Persistence: distinct crossed episodes and the longest run. The
   synthetic session's longest episode recovers the injected 400 ms lead
   exactly, which is the closed-loop check that the episode bookkeeping
   itself is right.
3. Depth: how many cents the crossed side clears (mean / max).
4. Edge at the touch: depth times the smaller touch size, in dollars. The
   most one taker order at the best levels could capture.
5. Full-depth sweep: walk the richer book's bids against the cheaper
   book's asks to exhaustion. Sweep >= touch always; equality means the
   cross never ran past the top of book.
6. Fee netting: the Kalshi leg pays the taker fee (general schedule,
   ceil(0.07 * C * P * (1-P)); assumptions in src/model/fees.h), the
   Polymarket leg is free. This is where "crossable" stops implying
   "profitable".
7. Optimal fee-aware sweep: take only the fills that clear their own fee.
   Never negative (a taker declines losing fills), never above the gross
   sweep.
8. Reaction-latency survival: the optimal sweep still standing 50/100/250
   ms after an episode opens, standing-edge semantics (the value from the
   last update at or before that instant). Expired episodes count zero.

Synthetic-session numbers (steps 5000, lead 400 ms; regenerate with the
commands above - these are also what scripts/perf_gate.sh checks
structurally on every commit):

    edge at the touch      mean $1.13   max $3.00   per crossed update
    net of taker fees      mean -$0.36  max +$1.45  (109/537 profitable)
    optimal fee-aware      mean +$0.08  max +$1.45  (declining losers flips
    sweep                                            the sign)
    surviving a reaction   open $0.04, 50 ms $0.03 (107/210 episodes),
    delay                  100 ms $0.03 (59/210), 250 ms $0.01 (8/210)

The one-line story: the screen shows $3.00; a fee-paying taker with a
100 ms reaction keeps an expected $0.03, and by 250 ms only 8 of 210
windows still exist. The edge is real and it rots in about a quarter of
a second.

## Real-data basket coherence (the live capture)

Mutually exclusive outcomes obey a hard no-arbitrage bound whether or not
the basket lists every outcome: if the best BIDS ever sum above $1.00,
selling one contract of each locks in riskless profit (at most one pays).
The mid-price sum is the softer read: the venue's probability mass on the
listed outcomes. `basket = "..."` in configs/contracts.toml declares the
groups; sums are sampled only when every member is two-sided.

On the committed 30-minute live capture (replay with
configs/contracts.toml):

    basket fed-2026-07 (2 outcomes, polymarket):
      mid-sum mean $0.914 [$0.905..$1.400]
      bid-sum max $0.900 ($0.100 below the $1 arb bound), 2,568 samples
    basket wc26-winner (12 outcomes, polymarket):
      never fully quoted; at most 11 of 12 outcomes two-sided at once

The split between those two lines is the finding: mid-price coherence
BROKE during the session (a momentary $1.40 sum, from a thin book's mid
spiking), while the tradable bound never came within ten cents of
breaking. Quote noise and executable opportunity are different things,
which is the same lesson the fee ladder teaches on the synthetic side -
and here it is on live data.

## Where the mid is not fair value (live capture)

Every basis number above is built on mid prices. The mid ignores how much
size is standing on each side, and the size-weighted microprice does not:

    microprice = (bid * ask_size + ask * bid_size) / (bid_size + ask_size)

Each price is weighted by the OPPOSITE side's size, because a heavy bid
queue means the next trade is likelier to lift the offer than to hit the
bid. Queue imbalance, (bid_size - ask_size) / total, is the same
information as a number in [-1, 1].

On the committed 30-minute capture, 13 Polymarket events were two-sided:

    bid-heavy events        11 of 13      (mean imbalance +0.44)
    mean microprice - mid   +7.99 cents
    mean spread             23.8 cents

    wide books  (spread > 20c)   +9.88c gap over 10 events
    tight books (spread <= 20c)  +1.72c gap over 3 events

The gap scales with the spread, which is the honest reading: on a book
quoted 30 cents wide with a lopsided queue, the mid is not an estimate of
fair value, it is the middle of a gap nobody is trading in. The
size-weighted price sits about ten cents higher. A mid-based basis on
those markets is measuring quoting artifacts as much as value, and the
tight-book events are where the mid-based numbers can be trusted.

This does not invalidate the crossable-dislocation ladder, which never
used mids: crossable requires one venue's best BID above the other's best
ASK, both executable prices. It qualifies the divergence statistics, and
the report now prints the microprice basis beside the mid basis so the
two can be compared per event.

Real-capture versions of the cross-venue numbers wait on the simultaneous
both-venue recording (Kalshi credentials); the pipeline and gates run
unchanged on a live feedlog.
