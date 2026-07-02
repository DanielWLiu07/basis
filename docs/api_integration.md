# Venue API integration notes

Concrete WebSocket details for Phase 1/2, validated against the live docs
(2026-06). This is the code-facing companion to PLAN.md. The asymmetries listed
at the bottom ARE the normalization problem the project sells on; keep them
visible.

## Kalshi

- **WSS**: `wss://external-api-ws.kalshi.com/trade-api/ws/v2`
  (demo: `wss://external-api-ws.demo.kalshi.co/trade-api/ws/v2`)
- **Auth: required, even for market data.** RSA-PSS / SHA-256 signature over
  `timestamp + "GET" + "/trade-api/ws/v2"`, sent as headers:
  - `KALSHI-ACCESS-KEY` (API key id)
  - `KALSHI-ACCESS-SIGNATURE` (base64 RSA-PSS signature)
  - `KALSHI-ACCESS-TIMESTAMP` (unix ms)
  The session is authenticated, but `orderbook_delta` returns the full public
  book for the subscribed markets (not user-specific data).
- **Subscribe**:
  ```json
  { "id": 2, "cmd": "subscribe",
    "params": { "channels": ["orderbook_delta"],
                "market_tickers": ["MARKET_TICKER"] } }
  ```
- **Messages**:
  - `orderbook_snapshot` - full book, keyed by `market_ticker`.
  - `orderbook_delta` - incremental change, keyed by `market_ticker`.
  - Stream carries a sequence number; a gap means we missed a delta -> drop the
    local book and re-snapshot. (Reliability requirement in PLAN.md.)
- **Prices**: integer cents, 1..99 (already our canonical unit).

**Prerequisite (action item):** a Kalshi account + generated API key (RSA key
pair). Free to create. The key is loaded from env / a local-only file, never
committed (.gitignore covers `.env`, `secrets/`).

## Polymarket

- **WSS**: `wss://ws-subscriptions-clob.polymarket.com/ws/market`
- **Auth: none** for the market channel (public).
- **Subscribe**:
  ```json
  { "assets_ids": ["<token_id>"], "type": "market" }
  ```
  `initial_dump` defaults true (snapshot on subscribe); `level` defaults 2.
- **Messages**:
  - `book` - snapshot: `bids`/`asks` as arrays of `{price, size}` (strings),
    plus `asset_id`, `market` (condition id), `timestamp` (ms), `hash`.
  - `price_change` - delta: `price_changes[]` each with `asset_id`, `price`,
    `size` (string; `"0"` = level removed), `side` (BUY/SELL), `hash`, optional
    `best_bid`/`best_ask`; plus top-level `market`, `timestamp`.
  - Integrity is by `hash`, not a sequence number -> on hash mismatch,
    re-subscribe / re-snapshot. (Different gap-recovery mechanism than Kalshi.)
- **Identification**: `market` = condition id (0x... hash); `asset_id` = token
  id for one outcome. A binary market has two tokens (YES/NO); we track the YES
  token and derive NO = 1 - YES.
- **Prices**: decimal probability strings, `"0.00".."1.00"`. Convert to canonical
  cents: `round(price * 100)`.

## The asymmetries (this is the normalization layer)

| Dimension        | Kalshi                          | Polymarket                          |
|------------------|---------------------------------|-------------------------------------|
| Auth             | RSA-signed session (required)   | none (public)                       |
| Price unit       | integer cents 1..99             | decimal prob string "0.47"          |
| Identifier       | `market_ticker` (string)        | `market` (condition id) + `asset_id` (token) |
| Outcome model    | one ticker per contract         | two tokens (YES/NO) per market       |
| Snapshot/delta   | `orderbook_snapshot` + `orderbook_delta` | `book` + `price_change`     |
| Gap recovery     | sequence number gap -> resnapshot | hash mismatch -> resnapshot        |
| Side encoding    | yes/no book                     | BUY/SELL on a token                 |
| Timestamp        | (per message)                   | unix ms string                      |

Normalizer's job (Phase 3): collapse all of the above into one `model::BookDelta`
stream keyed by a venue-neutral event id, with prices in canonical cents and a
single bid/ask convention. The contract registry maps `kalshi.market_ticker`
and `polymarket.token_id` to the shared event id.

## Contract matching

`configs/contracts.toml` stores, per shared event: the Kalshi `market_ticker`
and the Polymarket `token_id` (the YES outcome's asset id). Real ids get filled
once both feeds are live and overlapping markets confirmed (target categories
with genuine overlap: Fed/macro, elections, major sports).
