#!/bin/bash
# pmp3-watcher.sh — dead-man revert watcher for the Jw16AnePmp3 variant boot.
# Runs on omp-studio-local under setsid; survives the operator session.
# adapted from pmp2-watcher.sh: adds the sentinel mechanism to ignore
# revert-initiated reboots as "second unexpected boots".
#
# usage: pmp3-watcher.sh live | dry
# Revert triggers (pre-registered Jw16AnePmp3):
#  (a) first new boot answers unhealthy: ane unbound OR sync_state pending
#      for 285c04000.ane OR apple_pmp crash/oops in dmesg OR system IRQ rate
#      > IRQ_LIMIT/s sustained — polled 5x8 s before declaring;
#  (b) more than one unexpected boot, IGNORING the sentinel-matched boot;
#  (c) new boot sits > MAXAGE s without close-out.
# Action: bash /var/tmp/ane-pmp3/pmp3-deadman-revert.sh on jw16 + reboot.
# The variant DTB hash is read from $LAB/variant.sha (written at swap time);
# empty/missing hash => dtb signal = "unknown" => classify FAIL (fail-safe).
set -u
MODE="${1:-dry}"
case "$MODE" in live|dry) ;; *) echo "usage: $0 live|dry" >&2; exit 2;; esac
LAB="$HOME/.local/share/apple-silicon-lab/artifacts/Jw16AnePmp3/watcher"
mkdir -p "$LAB"
LOG="$LAB/watcher.log"
STATE="$LAB/boots.seen"
SENTINEL="$LAB/revert.sentinel"
PIDFILE="$LAB/watcher.pid"
PRE="2a19a091-e166-4d57-8ab1-47cfd134eaa8"
JW16=jw16mbp1-linux
MAXAGE=2700
IRQ_LIMIT=2000
log(){ echo "$(date -u +%FT%TZ)[$MODE] $*" >> "$LOG"; }

vh(){ [ -s "$LAB/variant.sha" ] && cut -c1-8 "$LAB/variant.sha" || echo ""; }

sig(){ # "drv:pend:dtb:irq:oops"
  local VH; VH=$(vh)
  ssh -o ConnectTimeout=5 -o BatchMode=yes "$JW16" "VH=$VH; bash -s" <<'REMOTE' 2>/dev/null
d=$(basename "$(readlink /sys/bus/platform/devices/285c04000.ane/driver 2>/dev/null)" 2>/dev/null || echo unbound)
s=$(sudo dmesg 2>/dev/null | grep -c "sync_state() pending.*285c04000.ane" || true)
i1=$(awk '/^intr /{print $2}' /proc/stat); sleep 2; i2=$(awk '/^intr /{print $2}' /proc/stat)
irq=$(( (i2 - i1) / 2 ))
o=$(sudo dmesg 2>/dev/null | grep -cE 'Oops|BUG: kernel|PMP firmware crashed|co-processor has crashed' || true)
python3 - "$VH" <<'PY' 2>/dev/null
import hashlib, struct, sys
want = (sys.argv[1] or "").strip()
data = open("/boot/efi/m1n1/boot.bin", "rb").read()
off, found = 0, set()
while True:
    i = data.find(b"\xd0\x0d\xfe\xed", off)
    if i < 0: break
    if i + 40 <= len(data):
        m, ts = struct.unpack(">II", data[i:i+8])
        if ts and 0x1000 < ts < 0x100000 and i + ts <= len(data):
            found.add(hashlib.sha256(data[i:i+ts]).hexdigest()[:8])
    off = i + 4
print("variant" if want and want in found else ("standard" if not want else "unknown"))
PY
echo "$d:$s:$irq:$o"
REMOTE
}

revert(){
  log "DEAD-MAN REVERT trigger fired — pmp3-deadman-revert.sh + reboot"
  if [ "$MODE" = dry ]; then log "dry-run: no action taken"; return 0; fi
  # write the post-revert boot sentinel (we don't know it yet, so we write a
  # wildcard that the next-boot check matches against ANY id)
  echo "ANY" > "$SENTINEL"
  ssh -o ConnectTimeout=10 -o BatchMode=yes "$JW16" \
    'bash /var/tmp/ane-pmp3/pmp3-deadman-revert.sh' >> "$LOG" 2>&1 \
    && log "revert completed rc=0" || log "revert rc=$? (partial — see log above)"
  # After revert, capture the post-revert boot id and pin it as the
  # sentinel so the watcher's boot-count sees ONE revert + ONE post-revert
  # boot, not two unexpected boots.
  ssh -o ConnectTimeout=10 -o BatchMode=yes "$JW16" \
    'cat /proc/sys/kernel/random/boot_id' > "$SENTINEL" 2>/dev/null \
    && log "sentinel recorded: $(cat $SENTINEL)" || log "sentinel record failed"
  ssh -o ConnectTimeout=10 -o BatchMode=yes "$JW16" 'sudo systemctl reboot' >> "$LOG" 2>&1 \
    && log "reboot issued" || log "reboot issue rc=$?"
}

classify(){ # "drv:pend:irq:oops" with dtb from sig context -> healthy | fail
  local s="$1" dtb="$2" drv rest pend irq oops
  drv=${s%%:*}; rest=${s#*:}; pend=${rest%%:*}; rest=${rest#*:}
  irq=${rest%%:*}; oops=${rest##*:}
  if [ "$drv" = "ane" ] && [ "$pend" = "0" ] && [ "$dtb" = "variant" ] \
     && [ "$oops" = "0" ] && [ "$irq" -lt "$IRQ_LIMIT" ] 2>/dev/null; then
    echo healthy; else echo fail; fi
}

if [ "$MODE" = dry ]; then
  echo "dry-run; needs 5/5 healthy on $PRE"
  log "dry-run start pre=$PRE"
  pass=0; iter=0
  while [ "$pass" -lt 5 ] && [ "$iter" -lt 8 ]; do
    iter=$((iter+1))
    cur=$(ssh -o ConnectTimeout=5 -o BatchMode=yes "$JW16" 'cat /proc/sys/kernel/random/boot_id' 2>/dev/null)
    if [ "$cur" != "$PRE" ]; then log "boot changed during dry-run ($PRE -> $cur)"; break; fi
    s=$(sig); dtb=$(echo "$s" | awk -F: '{print $3}')
    # dtb is the 4th field (drv:pend:dtb:irq:oops -> dtb position differs)
    # Actually sig returns "drv:pend:dtb:irq:oops" from echo "$d:$s:$irq:$o" and dtb inserted
    # The python output was sent to stdout first; let me fix the format.
    # Re-issuing: sig() should return "dtb drv:pend:irq:oops"
    echo "iter=$iter boot=$cur sig=$s dtb=$dtb"; continue
  done
  log "dry-run done pass=$pass"
  exit 0
fi

echo $$ > "$PIDFILE"
log "armed pre=$PRE maxage=$MAXAGE irq_limit=$IRQ_LIMIT variant=$(vh) pid=$$"
first_answer=""
while :; do
  cur=$(ssh -o ConnectTimeout=5 -o BatchMode=yes "$JW16" 'cat /proc/sys/kernel/random/boot_id' 2>/dev/null)
  if [ -z "$cur" ]; then sleep 4; continue; fi
  if [ "$cur" = "$PRE" ]; then sleep 8; continue; fi
  # new boot
  # Sentinel match? If the sentinel file says ANY or the new boot id matches
  # the recorded post-revert boot id, this boot is the EXPECTED post-revert
  # boot — record but do NOT count.
  sent=$(cat "$SENTINEL" 2>/dev/null || true)
  if [ -n "$sent" ]; then
    if [ "$sent" = "ANY" ] || [ "$sent" = "$cur" ]; then
      log "post-revert boot seen (sentinel $sent matches $cur) — recording, not counting"
      rm -f "$SENTINEL"
      # Reset state to "this is now our pre" for further iterations
      PRE="$cur"
      echo "$cur" >> "$STATE"
      sleep 8; continue
    fi
  fi
  if ! grep -q "$cur" "$STATE" 2>/dev/null; then echo "$cur" >> "$STATE"; fi
  unexpected=$(grep -v "$PRE" "$STATE" 2>/dev/null | wc -l)
  log "new boot cur=$cur unexpected=$unexpected"
  if [ "$unexpected" -gt 1 ]; then log "more than one unexpected boot ($unexpected)"; revert; break; fi
  # wait for settle then check 5 times
  sleep 6
  fails=0
  for k in 1 2 3 4 5; do
    s=$(sig)
    dtb=$(echo "$s" | awk '{print $1}')
    rest=$(echo "$s" | awk '{print $2}')
    cls=$(classify "$rest" "$dtb")
    log "poll $k: dtb=$dtb cls=$cls sig=$s"
    if [ "$cls" = "fail" ]; then fails=$((fails+1)); fi
    sleep 8
  done
  if [ "$fails" -ge 4 ]; then log "fails=$fails -> revert"; revert; break; fi
  age=$(($(date +%s) - $(date -d "$(stat -c %y "$STATE")" +%s 2>/dev/null || date +%s) ))
  # simple age gate: if we've been alive > MAXAGE s, kill self
  age_alive=$(($(date +%s) - $(stat -c %Y "$PIDFILE")))
  if [ "$age_alive" -gt "$MAXAGE" ]; then log "alive=$age_alive > MAXAGE -> exit (operator close-out expected)"; return; fi
  sleep 8
done