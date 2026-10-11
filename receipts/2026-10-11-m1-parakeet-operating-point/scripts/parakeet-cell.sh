#!/bin/bash
# run.sh : H13 ANE ticket (13-inch M1 T8103, or the M1 Max). Whole Parakeet encoder, one H13 program (Apple compiled, shipped by omarchy-mlx), ONE ane-run
# process, 20 timed calls, after an idle. Records: wall per call (ane-run --time), engine busy time per call from the driver's
# ane_stats busy_ns (so the host part = wall - busy), output bit-exactness against the macOS gold, identities of every input.
# Same 20-call protocol as the 2026-09-22 receipt (whole-encoder hwx one submit, 20 reps). Rerun-safe. DRY=1: no device access.
set -u
DRY=${DRY:-0}
IDLE=${IDLE:-60}
ROOT=/var/tmp/m1slot
TOOLS=$ROOT/tree/tools
IN=$ROOT/pk-in
ANEC=${ANEC:-/usr/lib/omarchy-mlx/venv/lib/python3.14/site-packages/mlx/share/mlx-omarchy/parakeet-1/bundles/parakeet-encoder-whole/program-0.anec}
STATS=/sys/class/accel/accel0/device/ane_stats
REC=$ROOT/parakeet-m1-receipts.log
LOCK=/var/tmp/ane-run.lock
WANT_ANEC_BYTES=458018816
TILE_SHIFT=${TILE_SHIFT:-9}   # the hwxv2 whole-program container counts tiles in 512-byte units (2026-09-22 receipt)
OUT=$ROOT/parakeet-m1-$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$OUT"
exec > "$OUT/results.txt" 2>&1
echo "== parakeet whole encoder $(date -u +%FT%TZ) dry=$DRY kernel=$(uname -r) boot=$(cut -c1-8 /proc/sys/kernel/random/boot_id) chip_dt=$(tr -d "\\0" < /proc/device-tree/compatible 2>/dev/null | cut -c1-40)"
echo "module ane srcversion=$(modinfo -F srcversion ane 2>/dev/null) file=$(modinfo -F filename ane 2>/dev/null | sed 's#.*/lib/modules/##')"
echo "ane-run sha256 $(sha256sum "$TOOLS/ane-run" | cut -c1-64) (omarchy-ane main 75c8829 plus the --tile-shift patch); tile-shift $TILE_SHIFT"
echo "anec bytes $(stat -c %s "$ANEC") sha256 $(sha256sum "$ANEC" | cut -c1-64)"
echo "omarchy-mlx $(pacman -Q omarchy-mlx 2>/dev/null) numpy $(python3 -c 'import numpy;print(numpy.__version__)') blas $(pacman -Q blas-openblas 2>/dev/null)"
sha256sum "$IN"/*.f16 | sed 's#  .*/#  #' | cut -c1-80
fail() { echo "FAIL $*"; echo "$(date -u +%FT%TZ) rc=1 dry=$DRY out=$OUT $*" >> "$REC"; exit 1; }
[ "$(stat -c %s "$ANEC")" = "$WANT_ANEC_BYTES" ] || fail "anec size mismatch"
for f in "$TOOLS/ane-run" "$IN/mask.f16" "$IN/feat.f16" "$IN/gold-hidden.f16"; do [ -f "$f" ] || fail "missing $f"; done
if [ "$DRY" = 1 ]; then
	echo "DRY: would idle $IDLE s, then flock $LOCK ane-run --anec <anec> --in 0=mask.f16 --in 1=feat.f16 --out 0=hidden --out 1=mask_out --tile-shift $TILE_SHIFT --time --repeat 20; busy_ns read before and after"
	echo "$(date -u +%FT%TZ) rc=0 dry=1 out=$OUT" >> "$REC"
	exit 0
fi
echo "-- idle $IDLE s"; sleep "$IDLE"
snapshot_load() {
	echo "-- load snapshot ($1): dri holders: $(for q in /proc/[0-9]*; do ls -l "$q"/fd 2>/dev/null | grep -q "/dev/dri/" && echo -n "$(cat "$q"/comm) "; done)"
	echo "   top cpu: $(ps -eo pcpu,comm --sort=-pcpu | sed -n 2,5p | tr -s " " | tr "\n" ";")"
}
CAP=${CAP:-none}
cap_restore() {
	[ -n "${SAVED_BOOST:-}" ] || return 0
	for pol in /sys/devices/system/cpu/cpufreq/policy*; do
		sudo -n tee "$pol/scaling_max_freq" < "$OUT/saved-$(basename "$pol").max" > /dev/null
	done
	echo "$SAVED_BOOST" | sudo -n tee /sys/module/ane/parameters/boost_idle_ms > /dev/null
	echo "-- CAP restored: boost_idle_ms=$(cat /sys/module/ane/parameters/boost_idle_ms) $(for pol in /sys/devices/system/cpu/cpufreq/policy*; do echo -n "$(basename "$pol")max=$(cat "$pol/scaling_max_freq") "; done)"
	SAVED_BOOST=
}
trap cap_restore EXIT
if [ "$CAP" = low ]; then
	SAVED_BOOST=$(cat /sys/module/ane/parameters/boost_idle_ms)
	for pol in /sys/devices/system/cpu/cpufreq/policy*; do
		cat "$pol/scaling_max_freq" > "$OUT/saved-$(basename "$pol").max"
		sudo -n tee "$pol/scaling_max_freq" < "$pol/cpuinfo_min_freq" > /dev/null
	done
	echo 0 | sudo -n tee /sys/module/ane/parameters/boost_idle_ms > /dev/null
	sleep 1
	echo "-- CAP=low: boost_idle_ms=0, $(for pol in /sys/devices/system/cpu/cpufreq/policy*; do echo -n "$(basename "$pol")max=$(cat "$pol/scaling_max_freq") cur=$(cat "$pol/scaling_cur_freq") "; done)"
fi
snapshot_load before
LOAD=${LOAD:-none}
if [ "$LOAD" = cpu ]; then
	for i in 1 2 3 4 5 6 7 8; do timeout 8 sh -c "while :; do :; done" & done
	echo "-- LOAD=cpu: 8 busy loops started"; sleep 1
fi
b0=$(awk '/^busy_ns/{print $2}' "$STATS"); j0=$(awk '/^jobs/{print $2}' "$STATS")
echo "ane_stats before: $(tr '\n' ' ' < "$STATS" | cut -c1-200)"
bash "$ROOT/state-sampler.sh" "$OUT/state.csv" &
SAMP=$!
sleep 1
t0=$(date +%s.%N)
flock -w 600 "$LOCK" timeout 300 "$TOOLS/ane-run" --anec "$ANEC" --in "0=$IN/mask.f16" --in "1=$IN/feat.f16" \
	--out "0=$OUT/hidden.f16" --out "1=$OUT/mask_out.f16" --tile-shift "$TILE_SHIFT" --time --repeat 20 > "$OUT/exec.txt" 2>&1
rc=$?
t1=$(date +%s.%N)
sleep 1; touch "$OUT/state.csv.stop"; wait "$SAMP" 2>/dev/null; rm -f "$OUT/state.csv.stop"
b1=$(awk '/^busy_ns/{print $2}' "$STATS"); j1=$(awk '/^jobs/{print $2}' "$STATS")
echo "ane-run rc=$rc total_s=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.2f", b-a}')"
cat "$OUT/exec.txt"
snapshot_load after
cap_restore
echo "ane_stats after:  $(tr '\n' ' ' < "$STATS" | cut -c1-200)"
echo "busy_ns delta $((b1 - b0)) jobs delta $((j1 - j0))"
[ "$rc" -eq 0 ] || fail "ane-run rc=$rc"
python3 - "$OUT/hidden.f16" "$IN/gold-hidden.f16" "$((b1 - b0))" "$((j1 - j0))" <<'PY'
import sys, hashlib
import numpy as np
out = np.fromfile(sys.argv[1], dtype=np.float16)
gold = np.fromfile(sys.argv[2], dtype=np.float16)
n = gold.size
hid = out[:n]
mism = int((hid.view(np.uint16) != gold.view(np.uint16)).sum())
ulp = int(np.abs(hid.view(np.int16).astype(np.int32) - gold.view(np.int16).astype(np.int32)).max())
print(f"hidden: {n} fp16 words compared, mismatched words {mism}, max ulp delta {ulp}, sha256(first {n} words) {hashlib.sha256(hid.tobytes()).hexdigest()[:16]} gold {hashlib.sha256(gold.tobytes()).hexdigest()[:16]}")
print("BIT-EXACT" if mism == 0 else "NOT BIT-EXACT", "against the macOS gold")
busy, jobs = int(sys.argv[3]), int(sys.argv[4])
if jobs:
    print(f"engine busy per job {busy / jobs / 1e6:.2f} ms over {jobs} jobs")
PY
echo "$(date -u +%FT%TZ) rc=$? dry=$DRY out=$OUT" >> "$REC"
