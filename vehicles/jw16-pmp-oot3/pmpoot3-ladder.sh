#!/bin/bash
# pmpoot3-ladder.sh — PmpOot3 stage C5: map113 DVFS_ON probe + granted token
# ladder. GATED: runs only if the C1..C4 window produced an ack of any kind
# (Configure_Ack or PING acked); with no ack the ticket says record and skip.
# Accessor = the PROVEN Pmp5 pmp_dvfs.ko (fault-safe: the DVFS_ON enable abort
# surfaces as ioctl EFAULT with no Oops). Protocol per the earlier grant:
# DVFS_ON <- 1; idle 0x80000000; rungs 0x80000000|(prev<<4)|new one at a time,
# >=5 ms spacing (in-module), readback + report-SRAM rows after each rung,
# whole-encoder n1/n8 slope (gap7-bench.sh, hidden16 fca96f1355485ec3 bit-exact
# gate per rung), ladder <= state 5 (1500 MHz @ 909 mV), stop on
# no-improvement (>=0.97x best) / anomaly / thermal >=85 C / macOS-class
# n1 <= 150 ms (~138 expected). Restore = reverse rungs (prev<<4|0) + DVFS_ON
# <- 0, UNCONDITIONAL. PmpOot3: every line is appended+synced (no exec-tee).
# Run ON jw16 inside the gpuwin discipline (llm stopped, m1-gpu.lock held).
set -uo pipefail
TOP="${1:-5}"
DM=/var/tmp/pmpoot3
LOGD=$DM/logs
RUN_ID="pmpoot3-dvfs-$(date -u +%Y%m%dT%H%M%SZ)"
OUT="$LOGD/$RUN_ID"
mkdir -p "$OUT"
LOG="$LOGD/$RUN_ID.log"
: > "$LOG"
DEV=/dev/pmp_dvfs
RUNG=0x40044401; ON=0x00000002; OFF=0x00000003; RDCMD=0x80044404; RDON=0x80044405
HIDDEN_WANT=fca96f1355485ec3

log(){ echo "$(date -u +%FT%TZ) $*" >> "$LOG"; sync; }
crash_lines(){ sudo dmesg | grep -cE 'Oops|BUG: kernel|PMP firmware crashed|co-processor has crashed' || true; }
max_temp(){
	local m=0 z t
	for z in /sys/class/thermal/thermal_zone*/temp; do
		t=$(cat "$z" 2>/dev/null || echo 0)
		[ "$t" -gt "$m" ] && m=$t
	done
	echo "$m"
}
rows(){
	sudo insmod /var/tmp/pmp5-build/pmp5_report.ko 2>&1 | head -2
	sleep 0.2
	sudo dmesg | grep pmp5rep | grep -E 'report\.|ptd\.'
	sudo rmmod pmp5_report
}
bench(){ # n -> echoes the last gap7-bench line "n ms hidden mask rc"
	sudo /var/tmp/ane-perf/gap7-bench.sh "$OUT/bench" "$1" | tail -1
}
parse(){ echo "$1" | sed -n 's/.*elapsed_ms=\([0-9.]*\).*/\1/p'; }
parsehash(){ echo "$1" | sed -n 's/.*hidden=\([0-9a-f]*\).*/\1/p'; }

log "=== $RUN_ID boot=$(cat /proc/sys/kernel/random/boot_id) top=$TOP ==="

ACKS=$(sudo dmesg | grep -E 'Configure_Ack|PING acked' || true)
log "acks seen: ${ACKS:-NONE}"
if [ -z "$ACKS" ]; then
	log "C5-SKIP: no ack of any kind - ticket gates C5 on acks; still-locked evidence = no-ack"
	log "C5-SKIPPED"
	exit 0
fi

sudo insmod "$DM/pmp_dvfs.ko" || { log "FATAL: pmp_dvfs insmod failed"; exit 10; }
[ -e "$DEV" ] || { log "FATAL: $DEV missing"; exit 10; }

log "== initial map113 state =="
python3 - <<'PY'
import fcntl, os
RDCMD=0x80044404; RDON=0x80044405
fd=os.open("/dev/pmp_dvfs", os.O_RDWR)
print("initial CMD readback:", hex(fcntl.ioctl(fd, RDCMD, 0) & 0xffffffff))
print("initial ON  readback:", hex(fcntl.ioctl(fd, RDON, 0) & 0xffffffff))
os.close(fd)
PY
log "initial readbacks done"

log "== C5 part 1: DVFS_ON <- 1 (fault-safe accessor; Pmp5 discriminator) =="
set +e
ONRES=$(python3 - <<'PY'
import fcntl, os, time
ON=0x00000002; RDON=0x80044405
fd=os.open("/dev/pmp_dvfs", os.O_RDWR)
try:
	fcntl.ioctl(fd, ON, 0)
	print("ACCEPTED")
except OSError as e:
	print(f"REFUSED errno={e.errno}")
time.sleep(0.01)
rb=fcntl.ioctl(fd, RDON, 0) & 0xffffffff
print("ON-readback", hex(rb))
os.close(fd)
PY
)
set -e
log "$ONRES"
if ! grep -q ACCEPTED <<<"$ONRES"; then
	log "STILL-LOCKED: DVFS_ON enable refused even after acks ($(echo "$ONRES" | tr '\n' ' '))"
	sudo rmmod pmp_dvfs
	log "C5-LOCKED (no climbing; gate refusal recorded)"
	exit 0
fi

log "== C5 part 2: granted token ladder (top=$TOP) =="
log "ROWS pre-ladder:"; rows | while IFS= read -r l; do log "  $l"; done
T0=$(max_temp); log "pre-ladder max_temp=$T0"
baseline=$(bench 1);  log "BASELINE-n1: $baseline"
baseline8=$(bench 8); log "BASELINE-n8: $baseline8"
BEST=$(parse "$baseline"); [ -n "$BEST" ] || BEST=999999

IDLE=$(python3 - <<'PY'
import fcntl, os
RUNG=0x40044401; RDCMD=0x80044404
fd=os.open("/dev/pmp_dvfs", os.O_RDWR)
fcntl.ioctl(fd, RUNG, __import__('struct').pack('<I', 0))
print(hex(fcntl.ioctl(fd, RDCMD, 0) & 0xffffffff))
os.close(fd)
PY
)
log "idle token 0x80000000 written; CMD now: $IDLE"
rows | while IFS= read -r l; do log "  $l"; done

prev=0
STATE=climb
for new in $(seq 1 "$TOP"); do
	[ "$STATE" != climb ] && break
	CL=$(crash_lines); T=$(max_temp)
	if [ "$CL" != 0 ] || [ "$T" -ge 85000 ]; then log "ABORT pre-rung crash=$CL temp=$T"; STATE=abort; break; fi
	RB=$(python3 - "$prev" "$new" <<'PY'
import fcntl, os, struct, sys
RUNG=0x40044401; RDCMD=0x80044404
prev, new = int(sys.argv[1]), int(sys.argv[2])
fd=os.open("/dev/pmp_dvfs", os.O_RDWR)
fcntl.ioctl(fd, RUNG, struct.pack('<I', (prev<<4)|new))
print(hex(fcntl.ioctl(fd, RDCMD, 0) & 0xffffffff))
os.close(fd)
PY
)
	log "RUNG prev=$prev new=$new ioctl_rb=$RB"
	sleep 0.05
	rows | while IFS= read -r l; do log "  ROWS rung $new: $l"; done
	n1=$(bench 1);  log "RUNG${new}-n1: $n1"
	n8=$(bench 8);  log "RUNG${new}-n8: $n8"
	ms=$(parse "$n1"); h=$(parsehash "$n1")
	[ "$h" = "$HIDDEN_WANT" ] || { log "HASH-MISMATCH at rung $new: $h"; STATE=abort; break; }
	log "rung $new n1=$ms best=$BEST"
	if [ -n "$ms" ] && [ "$(python3 -c "print(1 if float('$ms') <= 150 else 0)")" = 1 ]; then
		log "STOP: macOS class reached (n1=$ms)"; STATE=done; break
	fi
	if [ -n "$ms" ] && [ "$(python3 -c "print(1 if float('$ms') >= float('$BEST')*0.97 else 0)")" = 1 ]; then
		log "STOP: no improvement (n1=$ms vs best=$BEST)"; STATE=done; break
	fi
	[ -n "$ms" ] && BEST=$ms
	prev=$new
done

log "--- restore (unconditional) ---"
python3 - "$prev" <<'PY'
import fcntl, os, struct, sys, time
RUNG=0x40044401; OFF=0x00000003; RDCMD=0x80044404; RDON=0x80044405
prev=int(sys.argv[1])
fd=os.open("/dev/pmp_dvfs", os.O_RDWR)
if prev:
	fcntl.ioctl(fd, RUNG, struct.pack('<I', (prev<<4)|0))
	print("restore token", hex((prev<<4)|0), "-> CMD:", hex(fcntl.ioctl(fd, RDCMD, 0) & 0xffffffff))
	time.sleep(0.05)
fcntl.ioctl(fd, OFF, 0)
print("DVFS_ON <- 0; readback:", hex(fcntl.ioctl(fd, RDON, 0) & 0xffffffff))
os.close(fd)
PY
log "restore ioctl done rc=$?"
rows | while IFS= read -r l; do log "  ROWS final: $l"; done
sudo rmmod pmp_dvfs && log "pmp_dvfs removed" || log "WARN: pmp_dvfs rmmod failed"
sync
log "=== $RUN_ID done state=$STATE ==="
