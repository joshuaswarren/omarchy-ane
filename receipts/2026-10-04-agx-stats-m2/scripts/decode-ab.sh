#!/bin/bash
# Paired decode A/B for the agx_stats overhead gate. Run on the M2 under the
# lane's GPU lock (gpu-turn ticket). CELL_CMD is the lane's existing d64 decode
# cell; it must print exactly one "<tok_s> <digest>" line per rep on stdout.
# Usage: CELL_CMD='...' decode-ab.sh <label> <reps> [reader-on-reps like 2,4]
# Output: <label>.tsv lines "<rep> <tok_s> <digest> quiet|reader".
set -euo pipefail
LABEL=${1:?label}
REPS=${2:?reps}
READER_REPS=${3:-}
[ -n "${CELL_CMD:-}" ] || { echo "CELL_CMD not set" >&2; exit 2; }

OUT="$LABEL.tsv"
: > "$OUT"
for i in $(seq 1 "$REPS"); do
  RPID=""
  case ",$READER_REPS," in *",$i,"*)
    F=$(ls /sys/class/drm/card*/device/agx_stats 2>/dev/null | head -1)
    if [ -n "$F" ]; then
      while :; do cat "$F" >/dev/null 2>&1 || break; sleep 0.1; done &
      RPID=$!
    fi ;;
  esac
  LINE=$(eval "$CELL_CMD" 2>&1 | tail -1)
  [ -z "$RPID" ] || kill "$RPID" 2>/dev/null || true
  TOK=$(printf '%s' "$LINE" | awk '{print $1}')
  DIG=$(printf '%s' "$LINE" | awk '{print $2}')
  case "$TOK" in ''|*[!0-9.]) echo "rep $i unparsable: $LINE" >&2; exit 1 ;; esac
  R=quiet
  case ",$READER_REPS," in *",$i,"*) R=reader ;; esac
  echo "$i $TOK $DIG $R" >> "$OUT"
done
cat "$OUT"
