#!/usr/bin/env bash
# Emits the threshold sweep in docs/bench/cross_venue_lead.md as markdown.
#
# The sweep is the point, not a formality: a single threshold can always be
# the one that happened to work. An effect that holds as the bar for "a
# repricing" rises, and weakens only as the event count falls, behaves like
# a real one. One that appears at a single threshold does not.
set -euo pipefail

BIN="${BIN:-build/src/basis}"
CAPTURE="${1:-docs/bench/btc-xvenue.feedlog}"
# A capture can hold several instruments; each is an independent
# experiment, so the sweep runs one at a time and says which.
INSTRUMENT="${2:-${INSTRUMENT:-BTC/USD}}"
THRESHOLDS="${THRESHOLDS:-25 50 100 200}"

if [[ ! -x "$BIN" ]]; then
  echo "no binary at $BIN (set BIN=...)" >&2
  exit 1
fi
if [[ ! -f "$CAPTURE" ]]; then
  echo "no capture at $CAPTURE" >&2
  exit 1
fi

echo "$INSTRUMENT"
echo
echo "| Move threshold | Binance moves | answered | Coinbase moves | answered | z | leader |"
echo "| ---: | ---: | ---: | ---: | ---: | ---: | :--- |"
for m in $THRESHOLDS; do
  out=$("$BIN" xvenue-lead "$CAPTURE" --move-cents "$m" --instrument "$INSTRUMENT")
  bm=$(sed -n 's/.*binance_moves=\([0-9]*\).*/\1/p' <<<"$out")
  ba=$(sed -n 's/.*binance_moves=[0-9]* answered=\([0-9]*\).*/\1/p' <<<"$out")
  br=$(sed -n 's/.*binance_moves=[0-9]* answered=[0-9]* rate=\([0-9.]*\).*/\1/p' <<<"$out")
  cm=$(sed -n 's/.*coinbase_moves=\([0-9]*\).*/\1/p' <<<"$out")
  ca=$(sed -n 's/.*coinbase_moves=[0-9]* answered=\([0-9]*\).*/\1/p' <<<"$out")
  cr=$(sed -n 's/.*coinbase_moves=[0-9]* answered=[0-9]* rate=\([0-9.]*\).*/\1/p' <<<"$out")
  z=$(sed -n 's/.*follow_rate_z=\([-0-9.]*\).*/\1/p' <<<"$out")
  led=$(sed -n 's/.*confirmed_leader=\([-0-9]*\).*/\1/p' <<<"$out")
  case "$led" in
    1) leader="Binance" ;;
    -1) leader="Coinbase" ;;
    *) leader="not resolved" ;;
  esac
  printf '| $%.2f | %s | %s (%s) | %s | %s (%s) | %s | %s |\n' \
    "$(bc -l <<<"$m/100")" "$bm" "$ba" "$br" "$cm" "$ca" "$cr" "$z" "$leader"
done
