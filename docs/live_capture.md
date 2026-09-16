# Recording a live capture

Everything in `docs/bench/` replays from a committed capture, so none of
it needs the network. This is how to take a new one.


With `-DBASIS_ENABLE_NET=ON` (needs system Boost and OpenSSL), `basis
record` connects to Polymarket's public market WebSocket, no credentials
required, subscribes to every contract in the registry, and captures the
raw feed:

```
cmake -B build-net -G Ninja -DBASIS_ENABLE_NET=ON
cmake --build build-net -j
./build-net/src/basis record captures/live.feedlog --seconds 60
./build-net/src/basis replay captures/live.feedlog --config configs/contracts.toml
```

There are two registries, and the split matters. `configs/contracts.toml`
maps the contracts the **committed captures** were recorded against, in
July 2026 - 2026 World Cup winners and the July Fed decision. All of them
have since resolved, so it captures nothing today and is kept only because
replaying `docs/bench/*.feedlog.gz` needs exactly those ids.

`configs/contracts-live.toml` is the one `record` and `live` default to,
generated from what is currently trading. It rots the same way, just
later, so check it before a capture:

```
scripts/contracts.py check configs/contracts-live.toml
scripts/contracts.py refresh -o configs/contracts-live.toml
```

That check exists because of how the rot presents: a registry of resolved
contracts and a market where nothing is trading both produce zero
messages, and nothing about an empty capture says which one happened. The
old registry went stale unnoticed and `basis live` printed one message in
twenty-five seconds before anyone asked why.

Kalshi requires an authenticated session even for market data (free
account + RSA API key). With credentials, the same command captures both
venues into one feedlog:

```
./build-net/src/basis record captures/live.feedlog --seconds 60 \
    --kalshi-key-id <your-key-id> --kalshi-pem secrets/kalshi.pem
```

The key file lives under gitignored `secrets/` and never enters the repo.
Until both venues stream, replay reports each event's one-sided book and
flags the missing overlap rather than inventing a basis.

`basis live` runs the analytics in real time instead of capturing: feed
IO threads hand owned deltas to a bounded queue drained by one analytics
thread, and per-event basis prints as the books move. The exit report
includes the queue accounting (in, out, high water, blocked pushes), so
zero message loss across the thread boundary is measured, not assumed:

```
./build-net/src/basis live --seconds 60
```
