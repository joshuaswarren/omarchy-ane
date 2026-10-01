#!/bin/bash
# Installed-path gate for the T6021 ANE: checks the install, then runs the
# selected fixture program through libane (ane-run) with seeded random
# sparse inputs (same surface shapes as the lab oracle inputs) and checks
# the output with ane-run --check OP. The Parakeet islands are checked by
# tools/island_ref.py instead (see below).
#
# usage: gate.sh OUTDIR [OP|--anec ANEC [--weights FILE]] [--insmod KO]
#   OUTDIR       output directory (created if missing)
#   OP           fixture op under fixtures/h14-anec (default add; one of
#                add mul relu add-scalar mul-scalar real-div-scalar
#                clip-low clip-high matvec, or an island: island-c-pv
#                island-a-kt island-a-attn-p1 island-b-select-runtime
#                island-b-select-constfill rms-c2048-gamma)
#   --anec ANEC  use a specific ANEC file instead of fixtures/h14-anec/<OP>
#   --weights F  fp16 [N, K] weight file for --check matvec (Qwen-dim
#                fixtures; required when the ANEC is a matvec program)
#   --insmod KO  load KO with no parameters first (for a boot where the
#                module is installed but not yet in modules.dep).
# Run from a fresh boot. The module cannot be unloaded, so this never rmmods.
set -euo pipefail
OUT=${1:?output dir required}
OP=${2:-add}
EXPLICIT_ANEC=""
WEIGHTS=""
KO=""
shift 2 || true
while [[ $# -gt 0 ]]; do
	case $1 in
	--anec) EXPLICIT_ANEC=${2:?anec path required}; shift 2;;
	--weights) WEIGHTS=${2:?weights path required}; shift 2;;
	--insmod) KO=${2:?module path required}; shift 2;;
	*) echo "unknown arg: $1" >&2; exit 2;;
	esac
done
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
FW=/lib/firmware/apple/ane/t602x_ane0_fw_selene_rc4x.macho
FW_SHA=a9c4b771294a6b115624d9480a6248d0899a1681a575e865070b87a3248427bc
if [[ -n $EXPLICIT_ANEC ]]; then
	ANEC=$EXPLICIT_ANEC
	[[ -f $ANEC ]] || { echo "no such anec: $ANEC"; exit 2; }
elif [[ -f $ROOT/fixtures/h14-anec/$OP/program-0.anec ]]; then
	ANEC=$ROOT/fixtures/h14-anec/$OP/program-0.anec
else
	# Island ANECs may live elsewhere (the original set was in
	# /var/tmp/islands-fixtures/ while the in-repo copy is staged).
	ANEC=""
fi
RUN=$ROOT/tools/ane-run
TRIALS=${TRIALS:-4}
TWO_IN=0
[[ $OP == add || $OP == mul ]] && TWO_IN=1
mkdir -p "$OUT"
exec > >(tee "$OUT/gate.log") 2>&1

echo "boot_id $(cat /proc/sys/kernel/random/boot_id)"
echo "kernel $(uname -r)"
echo "anec $ANEC"
[[ -z $WEIGHTS ]] || echo "weights $WEIGHTS"
printf '%s  %s\n' "$FW_SHA" "$FW" | sha256sum -c -
[[ -e /proc/device-tree/soc/ane@284000000 ]] || { echo "no ANE DT node"; exit 2; }
if [[ -n $KO ]]; then
	[[ ! -d /sys/module/ane_t6021 ]] || { echo "module already loaded"; exit 2; }
	sha256sum "$KO"
	timeout -k 5 90 sudo -n insmod "$KO"
fi
[[ -d /sys/module/ane_t6021 ]] || { echo "ane_t6021 not loaded (autoload failed)"; exit 3; }
for _ in {1..50}; do
	compgen -G '/dev/accel/accel*' >/dev/null && break
	sleep 0.2
done
ls -l /dev/accel/
dmesg | grep -iE 'ane_t6021|ane:' | tail -40 > "$OUT/dmesg-load.txt" || true

# Every island is checked by tools/island_ref.py: it packs the inputs in
# the channel layout of the island (row and plane strides, the device-proven
# select operand order, the rms gamma baked in kernel.bin beside the ANEC),
# runs the device through the installed ane-run at
# /var/tmp/inst/tools/ane-run, and compares with an fp64 reference (bmm:
# condition-normalized error <= 1.0; select: exact; rms: 2 ulp). The
# generic path below fills inputs densely and has no island reference.
case "$OP" in
island-*|rms-c2048-gamma)
	for s in 0 1 2; do
		python3 "$ROOT/tools/island_ref.py" --island "$OP" --seed "$s" \
			${ANEC:+--anec "$ANEC"} --out-dir "$OUT" --in-dir "$OUT" |
			tee -a "$OUT/gate.log" | tail -1
	done
	if grep -q "FAIL" "$OUT/gate.log"; then echo "GATE FAIL"; exit 1; fi
	echo "GATE PASS"
	exit 0
	;;
esac

fail=0
for t in $(seq 1 "$TRIALS"); do
	python3 - "$OUT" "$t" "$OP" "$TWO_IN" "$ANEC" <<'PY'
# Sparse inputs in the surface layout of the fixture (same shapes as the
# lab oracle inputs): elementwise ops are nchw [1,512,1,1] with a 64-byte
# plane stride (one valid fp16 per 64 bytes, index % 32 == 0); matvec is
# M rows of K dense halves (M and K from the ANEC nchw) in the padded
# buffer. Padding lanes stay zero; the ANE ignores them and writes zero.
import numpy as np, struct, sys
out, t, op, two_in = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4] == "1"
anec = sys.argv[5]
seed0 = {"add": 1000, "mul": 2000, "relu": 3000, "add-scalar": 4000,
         "mul-scalar": 5000, "real-div-scalar": 6000, "clip-low": 7000,
         "clip-high": 8000, "matvec": 9000}.get(op, 17000)
rng = np.random.default_rng(seed0 + t)

if op == "matvec":
    hdr = open(anec, "rb").read(0x1000)
    tiles = struct.unpack_from("<32I", hdr, 40)
    _, _, m, k, _, _ = struct.unpack_from("<6Q", hdr, 168 + 5 * 48)
    x = np.zeros(tiles[5] * 0x4000 // 2, dtype=np.float16)
    x[: m * k] = rng.uniform(-1, 1, m * k).astype(np.float16)
    x.tofile(f"{out}/in-a-{t}.fp16")
else:
    for name in ("a", "b")[: 2 if two_in else 1]:
        x = np.zeros(16384, dtype=np.float16)
        x[::32] = rng.uniform(-8, 8, 512).astype(np.float16)
        x.tofile(f"{out}/in-{name}-{t}.fp16")
PY
	if [[ $TWO_IN == 1 ]]; then
		INS=(--in 0="$OUT/in-a-$t.fp16" --in 1="$OUT/in-b-$t.fp16")
	else
		INS=(--in 0="$OUT/in-a-$t.fp16")
	fi
	if "$RUN" --anec "$ANEC" "${INS[@]}" \
		--out 0="$OUT/out-y-$t.fp16" --check "$OP" \
		${WEIGHTS:+--weights "$WEIGHTS"}; then
		echo "trial $t PASS"
	else
		echo "trial $t FAIL"; fail=1
	fi
done
# A second process in the same boot closes and reopens the device.
if [[ $TWO_IN == 1 ]]; then
	INS=(--in 0="$OUT/in-a-1.fp16" --in 1="$OUT/in-b-1.fp16")
else
	INS=(--in 0="$OUT/in-a-1.fp16")
fi
if "$RUN" --anec "$ANEC" "${INS[@]}" \
	--out 0="$OUT/out-y-reopen.fp16" --check "$OP" --repeat 3 \
	${WEIGHTS:+--weights "$WEIGHTS"}; then
	echo "reopen+repeat PASS"
else
	echo "reopen+repeat FAIL"; fail=1
fi
(cd "$OUT" && sha256sum ./*.fp16 gate.log > SHA256SUMS)
if [[ $fail -eq 0 ]]; then echo "GATE PASS"; else echo "GATE FAIL"; exit 1; fi
