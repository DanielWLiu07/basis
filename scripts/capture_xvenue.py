#!/usr/bin/env python3
"""Capture Binance and Coinbase quotes for one instrument into one feedlog.

The whole point of this script is the clock. Both venues are read by one
process and every message is stamped with time.time_ns() the instant recv
returns, so the two streams share a time base. Capturing them separately,
or trusting the venues' own timestamps, would put an unknown offset
straight into the lead being estimated.

What it cannot fix is network distance: the stamp includes however long
the message took to arrive, and the two venues are not equidistant. See
docs/bench/cross_venue_lead.md for that bias and its measured size.

Usage:
  python3 scripts/capture_xvenue.py out.feedlog 2700 [BTCUSDT BTC-USD]

Requires the `websockets` package. Output is the .feedlog format the
engine replays: recv_ns <TAB> venue <TAB> raw payload.
"""

import asyncio
import json
import sys
import time

import websockets

BINANCE_URL = "wss://stream.binance.com:9443/stream?streams={sym}@bookTicker"
# level2 now requires authentication; level2_batch does not, and coalesces
# updates on a 50 ms timer. That timer is a floor on what can be resolved.
COINBASE_URL = "wss://ws-feed.exchange.coinbase.com"


async def pump(url, subscribe, venue, out, lock, deadline, counts):
    while time.monotonic() < deadline:
        try:
            async with websockets.connect(
                url, ping_interval=20, max_size=32 * 1024 * 1024
            ) as ws:
                if subscribe:
                    await ws.send(json.dumps(subscribe))
                while time.monotonic() < deadline:
                    try:
                        msg = await asyncio.wait_for(ws.recv(), timeout=10)
                    except asyncio.TimeoutError:
                        continue
                    recv_ns = time.time_ns()
                    # The feedlog frames one record per line; a payload with
                    # an embedded newline would break framing, and outside
                    # JSON strings whitespace is insignificant.
                    flat = msg.replace("\n", " ").replace("\r", " ")
                    async with lock:
                        out.write(f"{recv_ns}\t{venue}\t{flat}\n")
                    counts[venue] += 1
        except Exception as exc:  # noqa: BLE001 - reconnect on anything
            print(f"{venue} reconnect after {type(exc).__name__}", flush=True)
            await asyncio.sleep(1)


async def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    path, seconds = sys.argv[1], float(sys.argv[2])
    binance_sym = sys.argv[3].lower() if len(sys.argv) > 3 else "btcusdt"
    coinbase_sym = sys.argv[4] if len(sys.argv) > 4 else "BTC-USD"

    deadline = time.monotonic() + seconds
    counts = {"binance": 0, "coinbase": 0}
    lock = asyncio.Lock()
    with open(path, "w") as out:
        await asyncio.gather(
            pump(BINANCE_URL.format(sym=binance_sym), None,
                 "binance", out, lock, deadline, counts),
            pump(COINBASE_URL,
                 {"type": "subscribe", "product_ids": [coinbase_sym],
                  "channels": ["level2_batch"]},
                 "coinbase", out, lock, deadline, counts),
        )
    print(f"wrote {path}: {counts}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()) or 0)
