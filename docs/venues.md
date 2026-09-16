# The venues, and which of them have actually run

The README carries the measured results. This is the context behind them:
why this pairing, what the registry is for, and exactly which venues have
streamed live rather than been implemented.

## What it was built for

The original target is a pair of prediction markets rather than two crypto
exchanges:

- **Kalshi** is a CFTC-regulated, USD-denominated, centralized exchange.
- **Polymarket** runs on crypto rails (USDC): an off-chain order book with
  on-chain settlement on Polygon.

They list contracts on the same outcomes with different participant bases,
capital efficiency, and settlement rails, so their prices diverge and one
tends to move first. That pairing is why the engine normalizes across
venues at all, and the contract registry maps real cross-venue contracts
between Kalshi tickers and Polymarket token ids.

Prediction-market contracts resolve, so a registry of them has a shelf
life. The 14 in `configs/contracts.toml` were matched in July 2026 and
have all since settled; that file is kept because the committed captures
carry its token ids. `configs/contracts-live.toml` is the current one and
`scripts/contracts.py check` says whether it still is.

**Both venues are now captured, and the first time Kalshi connected it
found a bug.** Polymarket's market channel is public. Kalshi requires an
authenticated session even for market data, and for most of this repo's
life it had no account, so the adapter was verified offline down to the
RSA-PSS signature and never run. When a key finally arrived the handshake
worked on the first attempt and the books came back empty: Kalshi had
migrated its wire format to decimal dollar strings under new field names,
and the parser treated the missing fields as a legal empty book. Every
snapshot parsed, every book was empty, and the malformed counter read
zero. `docs/bench/cross_venue_fomc.md` is the capture that came out of
fixing it, and the failure is written up in
[`docs/postmortems.md`](postmortems.md).

The crypto pairing carries the headline lead-lag result anyway, because
Binance and Coinbase quote continuously while these prediction markets go
minutes between updates.

That distinction is kept sharp everywhere in this repo: a figure is either
measured on a committed capture or it is labelled as not yet measured.

One scope limit belongs next to it, and it is half closed. The engine
carried price levels and no trade prints, which made it a book engine
rather than a market-data one. `model::Trade` now exists and the Coinbase
parser emits prints from both `ticker` and `match`, with the aggressor
inverted off the maker side the venue reports.

What is still missing is data, not code: the live feed subscribes to
`level2_batch` for depth, so **no committed capture contains a single
trade** and nothing in the analytics consumes one yet. Closing it needs a
channel change and a fresh capture.

![Normalized Polymarket mid prices from a 30 minute live capture](img/live-mids.png)

The engine's normalized view of real markets: 2026 World Cup winner books
from the committed 30 minute live capture (`docs/bench/latency.md`), venue
probability strings turned into one canonical cents-per-contract frame.
Figures regenerate from the committed capture with
`scripts/plot_bench.py`.
