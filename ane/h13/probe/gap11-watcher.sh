#!/bin/bash
# gap11-watcher.sh — dead-man revert watcher for the Gap11 jw16 variant boot.
# Runs on omp-studio-local under setsid; survives the operator session.
# usage: gap11-watcher.sh live | dry
# Revert triggers (pre-registered Gap11):
#  (a) first new boot answers with the failure signature: ane unbound OR
#      sync_state pending for 285c04000.ane OR variant DTB with a bad ane
#      signature — polled 5x8 s before declaring (ane binding lags boot);
#  (b) more than one unexpected boot (second new boot id) — the Gap9/10
#      hard-cut-then-comeback-in-same-variant-chain mode;
#  (c) the variant boot sits longer than MAXAGE s without close-out
#      (operator session lost; llm-inference stays down otherwise).
# Action: gap11-deadman-revert.sh on jw16 + reboot. Dry mode logs only.
set -u
MODE="${1:-dry}"
case "$MODE" in live|dry) ;; *) echo "usage: $0 live|dry" >&2; exit 2;; esac
LAB="$HOME/.local/share/apple-silicon-lab/artifacts/Jw16AneGap11/watcher"
mkdir -p "$LAB"
LOG="$LAB/watcher.log"
STATE="$LAB/boots.seen"
PIDFILE="$LAB/watcher.pid"
PRE="59135661-c2b8-432b-98b3-ed5aa279130d"
JW16=jw16mbp1-linux
MAXAGE=2700
log(){ echo "$(date -u +%FT%TZ)[$MODE] $*" >> "$LOG"; }

sig(){ # "drv:syncpend:dtbvariant"
  ssh -o ConnectTimeout=5 -o BatchMode=yes "$JW16" '
    d=$(basename "$(readlink /sys/bus/platform/devices/285c04000.ane/driver 2>/dev/null)" 2>/dev/null || echo unbound)
    s=$(sudo dmesg 2>/dev/null | grep -c "sync_state() pending.*285c04000.ane" || true)
    v=$(sudo python3 - <<"PY"
import hashlib, struct
try:
    data = open("/boot/efi/m1n1/boot.bin", "rb").read()
except OSError:
    print("ERR"); raise SystemExit
off, found = 0, set()
while True:
    i = data.find(b"\xd0\x0d\xfe\xed", off)
    if i < 0: break
    if i + 40 <= len(data):
        m, ts = struct.unpack(">II", data[i:i+8])
        if ts and 0x1000 < ts < 0x100000 and i + ts <= len(data):
            found.add(hashlib.sha256(data[i:i+ts]).hexdigest()[:8])
    off = i + 4
print("variant" if "96bcc324" in found else "standard")
PY
)
    echo "$d:$s:$v"' 2>/dev/null
}

revert(){
  log "DEAD-MAN REVERT trigger fired — gap11-deadman-revert.sh + reboot"
  if [ "$MODE" = dry ]; then log "dry-run: no action taken"; return 0; fi
  ssh -o ConnectTimeout=10 -o BatchMode=yes "$JW16" \
    'bash /var/tmp/ane-perf/gap11-deadman-revert.sh' >> "$LOG" 2>&1 \
    && log "revert completed rc=0" || log "revert rc=$? (partial — see log above)"
  ssh -o ConnectTimeout=10 -o BatchMode=yes "$JW16" 'sudo systemctl reboot' >> "$LOG" 2>&1 \
    && log "reboot issued" || log "reboot issue rc=$?"
}

# branch unit check used by the dry-run self-test (not the live loop)
classify(){ # $1 = "drv:pend:dtb" -> healthy | fail
  local s="$1" drv rest pend dtb
  drv=${s%%:*}; rest=${s#*:}; pend=${rest%%:*}; dtb=${rest##*:}
  if [ "$drv" = "ane" ] && [ "$pend" = "0" ]; then echo healthy; else echo fail; fi
}

echo $$ > "$PIDFILE"
log "armed pre=$PRE mode=$MODE maxage=$MAXAGE pid=$$"
first_answer=""
while :; do
  bid=$(ssh -o ConnectTimeout=5 -o BatchMode=yes "$JW16" 'cat /proc/sys/kernel/random/boot_id' 2>/dev/null) || { log "no-answer"; sleep 10; continue; }
  if [ "$bid" = "$PRE" ]; then first_answer=""; sleep 10; continue; fi
  if [ -f "$STATE" ] && grep -q "^$bid " "$STATE"; then
    # known new boot: trigger (c) max-age on the sitting variant boot
    if [ -n "$first_answer" ] && [ $(( $(date -u +%s) - first_answer )) -gt "$MAXAGE" ]; then
      log "boot $bid up > ${MAXAGE}s without close-out"
      revert
      first_answer=""
    fi
    sleep 10; continue
  fi
  echo "$bid $(date -u +%FT%TZ)" >> "$STATE"
  n=$(wc -l < "$STATE")
  log "new boot #$n id=$bid"
  first_answer="$(date -u +%s)"
  if [ "$n" -ge 2 ]; then
    log "trigger (b): more than one unexpected boot (n=$n)"
    revert
    sleep 30
    continue
  fi
  ok=""
  for i in 1 2 3 4 5; do
    sleep 8
    s=$(sig) || { log "sig query failed (poll $i)"; continue; }
    log "sig($i)=$s -> $(classify "$s")"
    [ "$(classify "$s")" = healthy ] && { ok=1; log "healthy new boot — no action, operator in control"; break; }
  done
  [ -z "$ok" ] && { log "trigger (a): variant-signature boot after 5 polls"; revert; }
  sleep 10
done
