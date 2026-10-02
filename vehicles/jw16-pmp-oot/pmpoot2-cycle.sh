#!/bin/bash
# pmpoot2-cycle.sh — STUDIO-SIDE driver for the single PmpOot2 variant boot:
# latch the watcher, reboot (fresh iBoot-staged PMP firmware — the only proven
# way to run it), identity gate, run the C1..C4 ladder window (NO rmmod), then
# the C5 map113 ladder. Close-out (revert + reboot + verify) is separate.
# usage: pmpoot2-cycle.sh [soak_secs] [ladder_top]
set -euo pipefail
SOAK="${1:-150}"
TOP="${2:-5}"
W=/tmp/pmpoot-watch-home
LABDIR="$W/.local/share/apple-silicon-lab/artifacts/Jw16AnePmp4/watcher"
JW16=jw16mbp1-linux

fail(){ echo "FATAL: $*"; exit 20; }
[ -f "$LABDIR/watcher.pid" ] || fail "watcher not armed"

echo "== latch watcher + refresh 45-min clock =="
date +%s > "$LABDIR/expect-reboot"
date +%s > "$LABDIR/variant.answered"

echo "== reboot $(date -u +%FT%TZ) =="
ssh -o ConnectTimeout=8 "$JW16" 'sudo systemctl reboot'

echo "== wait ssh (abort at 6 min: do NOT retry; ask for physical power cycle) =="
UP=""
for i in $(seq 1 36); do
  sleep 10
  if ssh -o ConnectTimeout=4 -o BatchMode=yes "$JW16" true 2>/dev/null; then
    UP=1; echo "ssh up after ~$((i*10))s"; break
  fi
done
[ -n "$UP" ] || fail "no ssh within 360 s — STOP per ticket (do not retry); physical power cycle needed"
sleep 8

echo "== identity =="
ssh "$JW16" 'sudo bash /var/tmp/pmpoot2/pmpoot-identity.sh' | tee /tmp/pmpoot2-identity-last.log
grep -q IDENTITY-OK /tmp/pmpoot2-identity-last.log || fail "identity gate"

echo "== stop llm-inference for the window (flock discipline) =="
ssh "$JW16" 'sudo systemctl stop llm-inference; sleep 2; systemctl is-active llm-inference || true'

echo "== ladder window (soak $SOAK s, extra: C1..C4, no rmmod) =="
ssh "$JW16" "sudo flock -w 900 /tmp/m1-gpu.lock bash /var/tmp/pmpoot2/pmpoot2-stage.sh $SOAK"
echo "== C5 map113 ladder (top $TOP, ack-gated) =="
ssh "$JW16" "sudo flock -w 900 /tmp/m1-gpu.lock bash /var/tmp/pmpoot2/pmpoot2-ladder.sh $TOP"
echo "== evidence pull =="
mkdir -p /tmp/pmpoot2-pull
scp -q "$JW16:/var/tmp/pmpoot2/logs/ladder-*.log" /tmp/pmpoot2-pull/ 2>/dev/null || true
scp -q "$JW16:/var/tmp/pmpoot2/logs/pmpoot2-dvfs-*.log" /tmp/pmpoot2-pull/ 2>/dev/null || true
ls -la /tmp/pmpoot2-pull/
echo "CYCLE-OK"
