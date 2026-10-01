#!/bin/bash
# pmp5-dvfs-run.sh — Jw16AnePmp5 Phase 2 per-rung ladder driver (variant boot).
# Pre-registered protocol (entry 20261001T134924Z, Q-3d): DVFS_ON <- 1; idle
# token; ONE rung at a time (prev<<4|new, prev=0 first), >=5 ms spacing in the
# module; after EACH climb rung: report-SRAM rows (pmp5_report.ko insmod/rmmod,
# read-only) + whole-encoder n1/n8 slope (gap7-bench.sh, hidden16
# fca96f1355485ec3 gate); never >1 rung; max state 5.
# Stop climbing on: no improvement vs best (>=3% worse), macOS class
# (n1 <= 150 ms), thermal >= 85 C, crash signature, or state 5 reached.
# ALWAYS restores (reverse rungs to 0, DVFS_ON <- 0 with readback) on exit.
# Run ON jw16 inside a gpuwin window (llm stopped, m1-gpu.lock held).
set -uo pipefail
RUN_ID="pmp5-dvfs-$(date -u +%Y%m%dT%H%M%SZ)"
LAB=/home/joshuawarren/.local/share/apple-silicon-lab/artifacts/Jw16AnePmp5/windows
OUT="$LAB/$RUN_ID"
mkdir -p "$OUT"
exec > >(tee -a "$OUT/run.log") 2>&1
DEV=/dev/pmp_dvfs
RUNG=0x40044401; ON=0x00000002; OFF=0x00000003; RDCMD=0x80044404; RDON=0x80044405
HIDDEN_WANT=fca96f1355485ec3
TOP=${1:-5}
echo "=== $RUN_ID boot=$(cat /proc/sys/kernel/random/boot_id) top=$TOP ==="
[ -e "$DEV" ] || { echo "FATAL $DEV missing"; exit 1; }

crash_lines() { sudo dmesg | grep -cE 'Oops|BUG: kernel|PMP firmware crashed|co-processor has crashed' || true; }
max_temp() {
	local m=0 z t
	for z in /sys/class/thermal/thermal_zone*/temp; do
		t=$(cat "$z" 2>/dev/null || echo 0)
		[ "$t" -gt "$m" ] && m=$t
	done
	echo "$m"
}
rows() {
	sudo insmod /var/tmp/pmp5-build/pmp5_report.ko 2>&1 | head -2
	sleep 0.2
	sudo dmesg | grep pmp5rep | grep -E 'report\.|ptd\.'
	sudo rmmod pmp5_report
}
bench() { # n -> echoes "n ms hidden mask rc"
	local n=$1 line ms hs msk rc
	line=$(sudo /var/tmp/ane-perf/gap7-bench.sh "$OUT/bench" "$n" | tail -1)
	echo "$line"
}
parse() { echo "$1" | sed -n 's/.*elapsed_ms=\([0-9.]*\).*/\1/p'; }
parsehash() { echo "$1" | sed -n 's/.*hidden=\([0-9a-f]*\).*/\1/p'; }

python3 - <<'PY'
import fcntl, os, struct, time
RUNG=0x40044401; ON=0x00000002; OFF=0x00000003; RDCMD=0x80044404; RDON=0x80044405
fd=os.open("/dev/pmp_dvfs", os.O_RDWR)
print("initial CMD readback:", hex(fcntl.ioctl(fd, RDCMD, 0) & 0xffffffff))
print("initial ON  readback:", hex(fcntl.ioctl(fd, RDON, 0) & 0xffffffff))
os.close(fd)
PY
rows
T0=$(max_temp); echo "pre-ladder max_temp=$T0"
baseline=$(bench 1); echo "BASELINE-n1: $baseline"
baseline8=$(bench 8); echo "BASELINE-n8: $baseline8"
BEST=$(parse "$baseline"); [ -n "$BEST" ] || BEST=999999
python3 - "$TOP" <<'PY'
import fcntl, os, struct
ON=0x00000002; RUNG=0x40044401; RDCMD=0x80044404
fd=os.open("/dev/pmp_dvfs", os.O_RDWR)
fcntl.ioctl(fd, ON, 0)
print("DVFS_ON <- 1")
time.sleep(0.01)
fcntl.ioctl(fd, RUNG, struct.pack('<I', 0))
print("idle token 0x80000000 written; CMD now:", hex(fcntl.ioctl(fd, RDCMD, 0) & 0xffffffff))
os.close(fd)
PY
rows
prev=0
STATE=climb
for new in $(seq 1 "$TOP"); do
	[ "$STATE" != climb ] && break
	CL=$(crash_lines); T=$(max_temp)
	if [ "$CL" != 0 ] || [ "$T" -ge 85000 ]; then echo "ABORT pre-rung crash=$CL temp=$T"; STATE=abort; break; fi
	line=$(python3 - "$prev" "$new" <<'PY'
import fcntl, os, struct, sys
RUNG=0x40044401; RDCMD=0x80044404
prev, new = int(sys.argv[1]), int(sys.argv[2])
fd=os.open("/dev/pmp_dvfs", os.O_RDWR)
fcntl.ioctl(fd, RUNG, struct.pack('<I', (prev<<4)|new))
print(hex(fcntl.ioctl(fd, RDCMD, 0) & 0xffffffff))
os.close(fd)
PY
	)
	echo "RUNG prev=$prev new=$new ioctl_rb=$line"
	sleep 0.05
	echo "ROWS after rung $new:"; rows
	n1=$(bench 1);  echo "RUNG$new-n1: $n1"
	n8=$(bench 8);  echo "RUNG$new-n8: $n8"
	ms=$(parse "$n1"); h=$(parsehash "$n1")
	[ "$h" = "$HIDDEN_WANT" ] || { echo "HASH-MISMATCH at rung $new: $h"; STATE=abort; break; }
	echo "rung $new n1=$ms best=$BEST"
	if [ -n "$ms" ] && [ "$(python3 -c "print(1 if float('$ms') <= 150 else 0)")" = 1 ]; then
		echo "STOP: macOS class reached (n1=$ms)"; STATE=done; break
	fi
	if [ -n "$ms" ] && [ "$(python3 -c "print(1 if float('$ms') >= float('$BEST')*0.97 else 0)")" = 1 ]; then
		echo "STOP: no improvement (n1=$ms vs best=$BEST)"; STATE=done; break
	fi
	[ -n "$ms" ] && BEST=$ms
	prev=$new
done
echo "--- restore ---"
python3 - "$prev" <<'PY'
import fcntl, os, struct, sys, time
RUNG=0x40044401; OFF=0x00000003; RDON=0x80044405; RDCMD=0x80044404
prev=int(sys.argv[1] or 0)
fd=os.open("/dev/pmp_dvfs", os.O_RDWR)
for new in reversed(range(0, prev)):
	fcntl.ioctl(fd, RUNG, struct.pack('<I', (prev<<4)|new))
	print(f"reverse rung {prev}->{new}; CMD now:", hex(fcntl.ioctl(fd, RDCMD, 0) & 0xffffffff))
	prev=new
	time.sleep(0.005)
fcntl.ioctl(fd, OFF, 0)
print("DVFS_ON <- 0 readback:", hex(fcntl.ioctl(fd, RDON, 0) & 0xffffffff))
os.close(fd)
PY
rows
echo "=== $RUN_ID done state=$STATE ==="
