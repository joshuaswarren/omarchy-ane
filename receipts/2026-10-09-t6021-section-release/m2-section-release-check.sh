#!/bin/sh
# T6021 section-release device check (ticket script — run inside the M2
# gpu-turn slot; userspace only: no module install, no reload, no reboot).
#
# Reads /sys/module/ane_t6021/parameters/bo_total_bytes around
#   stage 1: K loads of prog_006 in one ane-session (FREE between loads),
#   stage 2: a resident decode with two prompts, which forces three
#            configure passes (the shape that failed on boot 2105990f).
# Pass/fail criteria: receipts/2026-10-09-t6021-section-release/README.md.
set -eu

BT=/sys/module/ane_t6021/parameters/bo_total_bytes
LOCK=/var/tmp/ane-run.lock
QRES=${QRES:-/var/tmp/m2slot/qres/src/tools}
SESSION=${SESSION:-$QRES/ane-session}
DECODE=${DECODE:-$QRES/qwen_m2_decode.py}
MANIFEST=${MANIFEST:-/var/tmp/qwen-decode/manifest.json}
ANEC=${ANEC:-/var/tmp/qwen-real-anec-h14}
PORTS=${PORTS:-/var/tmp/qwen-conform-0e2c3743-r2}
K=${K:-8}
P6=${P6:-$ANEC/prog_006/program-0.anec}
OUT=${OUT:-/var/tmp/qwen-decode/section-release-$(date -u +%Y%m%dT%H%M%SZ)}

[ -e "$BT" ] || { echo "no ane_t6021 module loaded" >&2; exit 2; }
[ -e "$P6" ] || { echo "missing $P6" >&2; exit 2; }
P6PORTS=$PORTS/prog_006/ports.resolved.json
[ -e "$P6PORTS" ] || { echo "no ports.resolved.json for prog_006 under $PORTS" >&2; exit 2; }
mkdir -p "$OUT"

bo() { cat "$BT"; }

# Refuse to run when another job holds the ANE.
flock -n "$LOCK" true || { echo "ANE busy: $LOCK is held" >&2; exit 2; }

T0=$(bo)
echo "T0 before=$T0" | tee "$OUT/bo_total.tsv"

# Stage 1: K loads of the same program, FREE between loads, one session.
IN=$OUT/session.in
: > "$IN"
i=0
while [ "$i" -lt "$K" ]; do
	printf 'LOAD p %s %s\nFREE p\n' "$P6" "$P6PORTS" >> "$IN"
	i=$((i + 1))
done
printf 'QUIT\n' >> "$IN"
if flock "$LOCK" "$SESSION" --lock "$LOCK" \
	< "$IN" > "$OUT/session.out" 2> "$OUT/session.err"; then
	echo "stage 1: session ok"
else
	echo "stage 1: session FAILED (see $OUT/session.err)" >&2
	exit 1
fi
T1=$(bo)
echo "T1 after_${K}_same_program_loads=$T1" | tee -a "$OUT/bo_total.tsv"

# Stage 2: resident decode, two prompts -> three configure passes.
# No outer flock here: the resident session takes $LOCK itself around every CALL, so
# wrapping the decode in flock on the same file would deadlock it.
if python3 "$DECODE" \
	--manifest "$MANIFEST" --anec-dir "$ANEC" --ports-dir "$PORTS" \
	--ane-run "$QRES/ane-run" --session-bin "$SESSION" \
	--resident --prompts p001 p002 --new-tokens 1 \
	--out "$OUT/resident" > "$OUT/resident.log" 2> "$OUT/resident.err"; then
	echo "stage 2: resident decode ok"
else
	echo "stage 2: resident decode FAILED (see $OUT/resident.err," \
		"$OUT/resident.log)" >&2
	cat "$OUT/resident.log" >&2 || true
	exit 1
fi
T2=$(bo)
echo "T2 after_resident_configures=$T2" | tee -a "$OUT/bo_total.tsv"

# Stage 3: the same decode again, back to back, still no reboot.
if python3 "$DECODE" \
	--manifest "$MANIFEST" --anec-dir "$ANEC" --ports-dir "$PORTS" \
	--ane-run "$QRES/ane-run" --session-bin "$SESSION" \
	--resident --prompts p001 p002 --new-tokens 1 \
	--out "$OUT/resident-2" > "$OUT/resident-2.log" 2> "$OUT/resident-2.err"; then
	echo "stage 3: second resident decode ok"
else
	echo "stage 3: second resident decode FAILED" >&2
	exit 1
fi
T3=$(bo)
echo "T3 after_second_resident=$T3" | tee -a "$OUT/bo_total.tsv"

echo "section-release check complete: $OUT"
