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

Real-capture versions of these numbers wait on the simultaneous
both-venue recording (Kalshi credentials); the pipeline and gates run
unchanged on a live feedlog.
