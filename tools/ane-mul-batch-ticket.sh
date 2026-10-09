#!/bin/bash
# ane-mul-batch-ticket.sh - M2 one-ticket mul batch measurement (N=1,2,4,8).
#
# Phase 1 landing: batch_landing_check.py --op mul --n N --calls 12 per N
# (bit-exact vs the fp16 half-away mul reference, tools/ane-run.c CHK_MUL
# semantics). batch_landing_check.py takes /var/tmp/ane-run.lock itself;
# NEVER wrap it in another flock (that deadlock cost two boots).
# Phase 2 timed cells: 60 s idle before each cell, then 3 timed 10 s
# windows of flock-wrapped ane-run --repeat 200 on mul-batch-N (the lock is
# taken exactly once per window, like ane-batch-cost-sweep.sh, whose
# window/idle/fit machinery this copies; the original stays untouched),
# ane_stats busy_ns/jobs deltas per window, then least-squares
# t_call(N)=a+b*N fitted on 1/calls_per_s, printed next to the add fit
# (a=269us, b=12.2us/job; artifacts/AneBacklog/20261008T1910Z-m2-fixed-cost).
# dmesg is snapshotted before and after; new DART/fault/abort lines fail
# the ticket.
# DRY=1 makes no ANE call (landing skipped, windows sleep).
# Guard protocol: fill-run --once -- timeout MAXMIN m bash <this>; unique
# UTC outdir, self-redirect, one summary line into the receipt log.
set -u
DRY=${DRY:-0}
TREE=${TREE:-/var/tmp/m2slot/tree}
TOOLS=${TOOLS:-$TREE/tools}
WORK=${WORK:-/var/tmp/m2slot/work}
STATS=${STATS:-/sys/class/accel/accel0/device/ane_stats}
REC=${REC:-/var/tmp/m2slot/mul-batch-receipts.log}
NS="1 2 4 8"
OUT=${OUT:-/var/tmp/m2slot/mulbatch-$(date -u +%Y%m%dT%H%M%SZ)}
DM_BEFORE=$OUT/dmesg-before.txt
DM_AFTER=$OUT/dmesg-after.txt
mkdir -p "$OUT" "$WORK"
exec > "$OUT/results.txt" 2>&1
echo "== mul-batch-ticket $(date -u +%FT%TZ) dry=$DRY kernel=$(uname -r) \
module=$(cat /sys/module/ane_t6021/version 2>/dev/null) \
boot=$(cat /proc/sys/kernel/random/boot_id 2>/dev/null)"

for f in "$TOOLS/batch_landing_check.py" "$TOOLS/check_batch.py" \
	"$TOOLS/ane-run" "$TOOLS/gen-sized-inputs.py"; do
	[ -f "$f" ] || { echo "missing $f (stage per the ticket entry)"; exit 1; }
done
for n in $NS; do
	[ -f "$TREE/fixtures/h14-anec/mul-batch-$n/program-0.anec" ] || {
		echo "missing mul-batch-$n package"; exit 1; }
done
[ -f "$STATS" ] || { echo "no ane_stats at $STATS"; exit 1; }

dmesg_snap() {
	# dmesg may be restricted: sudo -n first, plain as fallback. Capture
	# via command substitution so the file stays user-owned (a root-owned
	# file here would break the second snapshot); empty = unavailable.
	local d
	d=$(sudo -n dmesg 2>/dev/null || dmesg 2>/dev/null) || true
	printf '%s\n' "$d" >"$1"
}
fault_count() {
	[ -s "$1" ] || { echo NA; return 0; }
	grep -icE "dart|fault|abort|iommu" "$1" || true
}
snap() {
	local t f
	f=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null)
	printf "  soc %s p0freq=%s\n" \
		"$(for t in /sys/class/hwmon/hwmon*/temp*_input; do [ -f "$t" ] && printf "%s=%s " "$(basename "$(dirname "$t")")/$(basename "$t")" "$(cat "$t")"; done)" "$f"
}
astat() { tr '\n' ' ' <"$STATS" 2>/dev/null; echo; }
idle60() {
	[ "$DRY" = 1 ] && { echo "  idle SKIPPED (DRY)"; return 0; }
	local end
	end=$(( $(date +%s) + 60 ))
	while [ "$(date +%s)" -lt "$end" ]; do sleep 5; done
	echo "  idle 60s done $(date -u +%FT%TZ)"
}
window() {
	local n=$1 end c=0 s0 s1 t0 t1
	[ -f "$WORK/in-$n-0.f16" ] || python3 "$TOOLS/gen-sized-inputs.py" "$n" || return 1
	s0=$(astat); t0=$(date +%s.%N)
	if [ "$DRY" = 1 ]; then
		sleep 2; c=$((100000 / n))
	else
		end=$(( $(date +%s) + 10 ))
		while [ "$(date +%s)" -lt "$end" ]; do
			flock -w 600 /var/tmp/ane-run.lock \
				"$TOOLS/ane-run" \
				--anec "$TREE/fixtures/h14-anec/mul-batch-$n/program-0.anec" \
				--in "0=$WORK/in-$n-0.f16" --in "1=$WORK/in-$n-1.f16" \
				--out "0=$WORK/out-bc-$n.f16" --repeat 200 >/dev/null || return 1
			c=$((c + 200))
		done
	fi
	t1=$(date +%s.%N); s1=$(astat)
	awk -v c="$c" -v t0="$t0" -v t1="$t1" -v s0="$s0" -v s1="$s1" -v n="$n" 'BEGIN {
		split(s0, x, " "); split(s1, y, " "); w = t1 - t0
		printf "N=%d calls=%d wall=%.1fs calls/s=%.0f busy_frac=%.4f jobs/s=%.1f bytes_per_call=%d", \
			n, c, w, c / w, (y[2] - x[2]) / (w * 1e9), (y[4] - x[4]) / w, 98304 * n }'
}

# ---- phase 1: landing (12 calls per N, self-locked; never flock-wrapped)
dmesg_snap "$DM_BEFORE"
landing=SKIPPED
for n in $NS; do
	if [ "$DRY" = 1 ]; then
		echo "-- landing n=$n SKIPPED (DRY)"
	else
		if python3 "$TOOLS/batch_landing_check.py" --op mul --n "$n" \
			--calls 12 --anec \
			"$TREE/fixtures/h14-anec/mul-batch-$n/program-0.anec"; then
			landing=PASS
		else
			echo "LANDING FAIL n=$n"
			dmesg_snap "$DM_AFTER"
			newf=$(comm -13 <(sort "$DM_BEFORE") <(sort "$DM_AFTER") \
				| grep -icE "dart|fault|abort|iommu" || true)
			echo "landing=FAIL dmesg_new_faults=$newf"
			echo "mul-batch-ticket $(date -u +%FT%TZ) dry=$DRY out=$OUT landing=FAIL" >>"$REC"
			exit 1
		fi
	fi
done
dmesg_snap "$DM_AFTER"
b=$(fault_count "$DM_BEFORE"); a=$(fault_count "$DM_AFTER")
echo "-- dmesg: fault-class lines before=$b after=$a"
if [ "$b" != "NA" ] && [ "$a" != "NA" ] && [ "$a" -gt "$b" ]; then
	echo "DMESG NOT CLEAN - new lines:"
	comm -13 <(sort "$DM_BEFORE") <(sort "$DM_AFTER") \
		| grep -iE "dart|fault|abort|iommu" | awk 'NR <= 10'
	echo "mul-batch-ticket $(date -u +%FT%TZ) dry=$DRY out=$OUT dmesg=NOT-CLEAN" >>"$REC"
	exit 1
fi
[ "$b" = "NA" ] && echo "DMESG UNAVAILABLE (could not read dmesg)"

# ---- phase 2: timed cells (3 windows per N, 60 s idle before each cell)
for n in $NS; do
	echo "-- cell N=$n $(date -u +%FT%TZ)"
	snap
	idle60
	snap
	rates=()
	for r in 1 2 3; do
		w=$(window "$n") || { echo "  window FAIL N=$n rep=$r"; exit 1; }
		echo "  rep$r $w"
		rates+=("$w")
	done
	snap
	mid=$(printf '%s' "${rates[1]}" | python3 -c '
import sys
c = f = 0.0
for t in sys.stdin.read().split():
    if t.startswith("calls/s="):
        c = float(t.split("=")[1])
    if t.startswith("busy_frac="):
        f = float(t.split("=")[1])
print(f"{c:.0f} {f:.4f}")')
	echo "$n $mid" >>"$OUT/cellmedians.txt"
done

fit_ok=1
python3 - "$OUT/cellmedians.txt" <<'PYEOF' || fit_ok=0
import sys
import numpy as np
rows = []
for line in open(sys.argv[1]):
    p = line.split()
    if len(p) >= 3:
        rows.append((int(p[0]), float(p[1]), float(p[2])))
if not rows:
    raise SystemExit("no cell medians")
rows.sort()
n = np.array([r[0] for r in rows], float)
cs = np.array([r[1] for r in rows], float)
bf = np.array([r[2] for r in rows], float)
for name, y in (("1/calls_per_s", 1.0 / cs), ("busy_frac/calls_per_s", bf / cs)):
    A = np.vstack([np.ones_like(n), n]).T
    (a, b), res, *_ = np.linalg.lstsq(A, y, rcond=None)
    pred = A @ np.array([a, b])
    rms = float(np.sqrt(np.mean((y - pred) ** 2)))
    print(f"fit[{name}]: a={a*1e6:.1f}us b={b*1e6:.1f}us/job rms_resid={rms*1e6:.1f}us")
    for ni, yi, pi in zip(n, y, pred):
        print(f"  N={int(ni)} t_call={yi*1e6:.1f}us pred={pi*1e6:.1f}us resid={(yi-pi)*1e6:+.1f}us")
    if name == "1/calls_per_s":
        t1, t2 = y[0] * 1e6, y[1] * 1e6
        print(f"  t(N=2)-t(N=1) = {t2 - t1:+.1f}us (pass rule: < 10us)")
        print(f"  slope b = {b*1e6:.1f}us/task (pass rule: 8-18)")
print("add reference fit (20261008T1910Z-m2-fixed-cost): a=269.0us b=12.2us/job")
PYEOF
if [ "$fit_ok" = 0 ]; then
	echo "FIT FAIL (windows produced no usable cells)"
	echo "mul-batch-ticket $(date -u +%FT%TZ) dry=$DRY out=$OUT landing=$landing fit=FAIL" >>"$REC"
	exit 1
fi
echo "== mul-batch-ticket COMPLETE $(date -u +%FT%TZ) landing=$landing fit=OK"
echo "mul-batch-ticket $(date -u +%FT%TZ) dry=$DRY out=$OUT landing=$landing fit=OK" >>"$REC"
