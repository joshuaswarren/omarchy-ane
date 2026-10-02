#!/bin/bash
# pmpoot3-cycle.sh — STUDIO-SIDE driver for the single PmpOot3 variant boot
# (EXP-1). PmpOot3 hazard fixes vs pmpoot2-cycle:
#   * NO pre-latch of variant.answered (the PmpOot2 dead-man fired on that
#     latch during the 130 s boot); only the expect-reboot latch is written.
#   * llm-benchmark-recovery.timer stopped BEFORE the window; re-armed only
#     after close-out llm verification (close-out script does that).
#   * stage script fsyncs every new module line (PmpOot2's logs stayed empty).
# usage: pmpoot3-cycle.sh [soak_secs] [ladder_top]
set -euo pipefail
SOAK="${1:-180}"
TOP="${2:-5}"
W=/tmp/pmpoot3-watch-home
LABDIR="$W/.local/share/apple-silicon-lab/artifacts/Jw16PmpOot3/watcher"
JW16=jw16mbp1-linux

fail(){ echo "FATAL: $*"; exit 20; }
[ -f "$LABDIR/watcher.pid" ] || fail "watcher not armed"

echo "== stop llm-benchmark-recovery.timer (PmpOot3 hazard fix b) =="
ssh "$JW16" 'sudo systemctl stop llm-benchmark-recovery.timer; systemctl is-active llm-benchmark-recovery.timer || true'

echo "== latch watcher (expect-reboot ONLY; variant.answered is set by real fields) =="
date +%s > "$LABDIR/expect-reboot"

echo "== reboot $(date -u +%FT%TZ) =="
ssh -o ConnectTimeout=8 "$JW16" 'sudo systemctl reboot'

echo "== wait ssh (abort at 360 s: do NOT retry; ask for physical power cycle) =="
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
ssh "$JW16" 'sudo bash /var/tmp/pmpoot3/pmpoot3-identity.sh' | tee /tmp/pmpoot3-identity-last.log
grep -q IDENTITY-OK /tmp/pmpoot3-identity-last.log || fail "identity gate"

echo "== stop llm-inference for the window (flock discipline) =="
ssh "$JW16" 'sudo systemctl stop llm-inference; sleep 2; systemctl is-active llm-inference || true'

echo "== ladder window (soak $SOAK s: C1 -> C2 -> ack-gated C4, one insmod, fsync logs) =="
ssh "$JW16" "flock -w 900 /tmp/m1-gpu.lock sudo bash /var/tmp/pmpoot3/pmpoot3-stage.sh $SOAK"
echo "== C5 map113 ladder (top $TOP, ack-gated) =="
ssh "$JW16" "flock -w 900 /tmp/m1-gpu.lock sudo bash /var/tmp/pmpoot3/pmpoot3-ladder.sh $TOP"
echo "== evidence pull =="
mkdir -p /tmp/pmpoot3-pull
scp -q "$JW16:/var/tmp/pmpoot3/logs/ladder-*.log" /tmp/pmpoot3-pull/ 2>/dev/null || true
scp -q "$JW16:/var/tmp/pmpoot3/logs/stages-*.txt" /tmp/pmpoot3-pull/ 2>/dev/null || true
scp -q "$JW16:/var/tmp/pmpoot3/logs/dmesg-*.txt" /tmp/pmpoot3-pull/ 2>/dev/null || true
scp -q "$JW16:/var/tmp/pmpoot3/logs/pmpoot3-dvfs-"* /tmp/pmpoot3-pull/ 2>/dev/null || true
ls -la /tmp/pmpoot3-pull/
echo "CYCLE-OK"
