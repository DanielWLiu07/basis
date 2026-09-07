# What the analytics actually compute

Pulled out of the README, which had grown a tail longer than its head.
Each item below is summarized from a `docs/bench/` writeup and links back
to it; nothing here is a separate measurement.

- Crossable dislocations are priced as a ladder, each rung answering the
  question the previous one raises: how often the books cross, how long
  the episodes persist, how deep they run, what one taker order at the
  touch captures, what sweeping the full crossed depth captures, and what
  survives Kalshi's taker fee (ceil(0.07 * C * P * (1-P)), assumptions in
  `src/model/fees.h`). On the synthetic session the ladder inverts the
  headline: the gross sweep averages $1.13 per crossed update, but only
  109 of 537 crossed updates survive fees (net at the touch: mean -$0.36),
  while a taker free to decline losing fills keeps mean +$0.08, max
  +$1.45 - and the survival ladder times the decay: 100 ms of reaction
  delay leaves an expected $0.03 (59/210 episodes still crossed), 250 ms
  leaves $0.01 (8/210). Crossable is not profitable; selective and fast
  is. The fee and sweep arithmetic saturates on corrupt sizes the same
  way the book does (UBSan-verified), so a bad feed cannot poison the
  economics. Methodology and regeneration commands:
  `docs/bench/economics.md`.
- The mid is not fair value on a wide book, and the engine says so with
  numbers: it tracks queue imbalance and the size-weighted microprice at
  every touch. On the committed 30-minute capture, 11 of 13 two-sided
  events were bid-heavy and the microprice sat 7.99 cents above the mid
  on average, scaling with the spread (+9.9c on books quoted wider than
  20 cents, +1.7c on tighter ones). That qualifies every mid-based
  divergence statistic and says which events they can be trusted on; the
  crossable-dislocation ladder is unaffected because it never used mids,
  only executable bid and ask prices (`docs/bench/economics.md`).
- The engine also has the venue side: `src/exec/limit_order_book.h` is a
  price-time-priority matching engine (Gtc/Ioc/Fok, O(1) submit, cancel
  and best-price via a flat 99-slot ladder and two-word occupancy bit
  scan, because prediction-market prices are integer cents 1..99). It
  sustains 42.6M operations/sec at 23.5 ns/op on an M4, 2.1x a textbook
  `std::map` book replaying the identical order flow. Two correctness
  checks run in CI: the two books must produce identical fill streams over
  200k random operations, and executing a sweep as order flow through the
  engine must reproduce `crossed_sweep_cents` exactly, so the executable
  edge numbers are cross-checked rather than trusted
  (`docs/bench/matching_engine.md`).
- The complementary YES/NO bound is monitored, an invariant that exists
  only because these contracts settle at $0 or $1: the two sides are
  worth exactly $1.00 together, so any gap is riskless. On the committed
  30-minute capture it effectively never broke. Getting that answer took
  three attempts, and the failures are the interesting part: comparing
  prices as doubles manufactured 267 phantom violations (0.53 + 0.47
  exceeds 1.0 in floating point), and sampling per delta rather than per
  wire message manufactured more from states that exist only midway
  through applying one message (`docs/bench/economics.md`).
- Mutually exclusive outcome groups are watched as baskets on live data:
  best-bid sums are checked against the hard $1 no-arbitrage bound (valid
  even for partial baskets), mid-price sums read the venue's probability
  mass. On the committed 30-minute capture the Fed basket's mid-sum broke
  coherence for a moment ($1.40) while the tradable bid-sum never came
  within ten cents of the bound - quote noise and executable opportunity
  are different things, measured on live data
  (`docs/bench/economics.md`).
- Venue integrity hashes are recomputed, not trusted: the parser rebuilds
  Polymarket's canonical book summary and checks its SHA-1 on every
  snapshot that carries the hashed fields, 13/13 verified with 0
  mismatches on the committed capture (docs/api_integration.md has the
  recipe).

A simultaneous both-venue recording now exists
(`docs/bench/cross_venue_fomc.md`), but it does not yet support a
prediction-market lead-lag figure: 218 Kalshi records in 23.6 minutes left
59.7% of samples priced against a quote more than five seconds old, so the
estimator reports no signal for want of data rather than want of effect.
