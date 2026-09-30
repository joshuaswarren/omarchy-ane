#!/bin/bash
# Installed-path gate for the T6021 ANE: checks the install, then runs the
# selected fixture program through libane (ane-run) with seeded random
# sparse inputs (same surface shapes as the lab oracle inputs) and checks
# the output with ane-run --check OP.
#
# usage: gate.sh OUTDIR [OP|--anec ANEC [--weights FILE]] [--insmod KO]
#   OUTDIR       output directory (created if missing)
#   OP           fixture op under fixtures/h14-anec (default add; one of
#                add mul relu add-scalar mul-scalar real-div-scalar
#                clip-low clip-high matvec)
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
# Island fixtures live next to the stage 1-4 ones, in their own tree.
ISLAND_FIXTURES=$ROOT/fixtures/h14-anec
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
# ane-run --check name for each gate op.
case "$OP" in
	island-a-kt|island-a-attn-p1|island-c-pv) CHECK=bmm;;
	island-b-select-runtime|island-b-select-constfill) CHECK=select;;
	rms-c2048-gamma) CHECK=rms;;
	*) CHECK=$OP;;
esac
[[ $OP == add || $OP == mul || $OP == bmm ]] && TWO_IN=1
[[ $OP == select ]] && THREE_IN=1 || THREE_IN=0
[[ $OP == rms ]] && ONE_IN=1 || ONE_IN=0
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

fail=0
for t in $(seq 1 "$TRIALS"); do
	python3 - "$OUT" "$t" "$OP" "$TWO_IN" "$THREE_IN" "$ANEC" <<'PY'
# Sparse inputs in the surface layout of the fixture (same shapes as the
# lab oracle inputs): elementwise ops are nchw [1,512,1,1] with a 64-byte
# plane stride (one valid fp16 per 64 bytes, index % 32 == 0); matvec is
# M rows of K dense halves (M and K from the ANEC nchw) in the padded
# buffer. Islands (bmm / select / rms) read their nchw from the ANEC
# header directly and emit one input per declared channel; padding lanes
# stay zero; the ANE ignores them and writes zero.
import numpy as np, struct, sys
out, t, op, two_in, three_in = (
    sys.argv[1], int(sys.argv[2]), sys.argv[3],
    sys.argv[4] == "1", sys.argv[5] == "1",
)
anec = sys.argv[6]
seed0 = {"add": 1000, "mul": 2000, "relu": 3000, "add-scalar": 4000,
         "mul-scalar": 5000, "real-div-scalar": 6000, "clip-low": 7000,
         "clip-high": 8000, "matvec": 9000,
         "island-a-kt": 11000, "island-a-attn-p1": 12000,
         "island-b-select-runtime": 13000,
         "island-b-select-constfill": 14000, "island-c-pv": 15000,
         "rms-c2048-gamma": 16000}.get(op, 17000)
rng = np.random.default_rng(seed0 + t)
hdr = open(anec, "rb").read(0x1000)
tiles = struct.unpack_from("<32I", hdr, 40)


def write_fp16_channel(path: str, ch: int, scale: float = 1.0,
			lo: float = -1.0, hi: float = 1.0) -> None:
    """Write a valid-lane fp16 input for ANEC channel ch.

    The channel's nchw (N,C,H,W,plane,row) is read from the header; the
    surface is (tiles[ch] * 0x4000) bytes. Valid lanes are dense within
    rows (the first W elements per row, all H rows, all C channels, all
    N batches); padding bytes inside the row (align_up(W*2, 64) - W*2)
    and the row tail past H*C*row_bytes stay zero. The ANE ignores
    padding and writes zero there.
    """
    n, c, h, w, _plane, row = struct.unpack_from("<6Q", hdr, 0xA8 + ch * 48)
    elem = n * c * h * w
    out16 = np.zeros(tiles[ch] * 0x4000 // 2, dtype=np.float16)
    out16[: elem] = rng.uniform(lo, hi, elem).astype(np.float16) * \
                    np.float16(scale)
    out16.tofile(path)


def write_bool_channel(path: str, ch: int) -> None:
    """Write a valid-lane bool input for ANEC channel ch. 1 byte/elem,
    row-aligned to 64 bytes (W bytes rounded up). Padding bytes inside
    the row (align_up(W, 64) - W) and the row tail past H*C*row_bytes
    stay zero.
    """
    n, c, h, w, _plane, row = struct.unpack_from("<6Q", hdr, 0xA8 + ch * 48)
    elem = n * c * h * w
    out8 = np.zeros(tiles[ch] * 0x4000, dtype=np.uint8)
    out8[: elem] = rng.integers(0, 2, elem).astype(np.uint8)
    out8.tofile(path)


if op == "matvec":
    _, _, m, k, _, _ = struct.unpack_from("<6Q", hdr, 168 + 5 * 48)
    x = np.zeros(tiles[5] * 0x4000 // 2, dtype=np.float16)
    x[: m * k] = rng.uniform(-1, 1, m * k).astype(np.float16)
    x.tofile(f"{out}/in-a-{t}.fp16")
elif op in ("island-a-kt", "island-c-pv"):
    write_fp16_channel(f"{out}/in-x-{t}.fp16", 5, lo=-0.5, hi=0.5)
    write_fp16_channel(f"{out}/in-y-{t}.fp16", 6, lo=-0.5, hi=0.5)
elif op == "island-a-attn-p1":
    write_fp16_channel(f"{out}/in-x-{t}.fp16", 5, lo=-0.5, hi=0.5)
    write_fp16_channel(f"{out}/in-y-{t}.fp16", 6, lo=-0.5, hi=0.5)
elif op == "island-b-select-runtime":
    write_fp16_channel(f"{out}/in-a-{t}.fp16", 5, lo=-1.0, hi=1.0)
    write_fp16_channel(f"{out}/in-b-{t}.fp16", 6, lo=-1.0, hi=1.0)
    write_bool_channel(f"{out}/in-c-{t}.bin", 7)
elif op == "island-b-select-constfill":
    # a is the runtime input (channel 5); b and cond are loaded from the
    # kernel section by the encoder, so the gate only generates a.
    write_fp16_channel(f"{out}/in-a-{t}.fp16", 5, lo=-1.0, hi=1.0)
elif op == "rms-c2048-gamma":
    write_fp16_channel(f"{out}/in-x-{t}.fp16", 5, lo=-1.0, hi=1.0)
    # Build a fake weights.bin so the rms check can be exercised: 64
    # bytes of sub-header + 2048 fp16 gamma values in [-1, 1].
    weights = np.zeros(64 // 2, dtype=np.float16)
    gamma = rng.uniform(-1, 1, 2048).astype(np.float16)
    np.concatenate([weights, gamma]).tofile(f"{out}/weights-{t}.bin")
else:
    for name in ("a", "b")[: 2 if two_in else 1]:
        x = np.zeros(16384, dtype=np.float16)
        x[::32] = rng.uniform(-8, 8, 512).astype(np.float16)
        x.tofile(f"{out}/in-{name}-{t}.fp16")
PY
	# Compose --in args per op.
	case "$OP" in
		island-a-kt|island-c-pv|island-a-attn-p1|bmm)
			INS=(--in 0="$OUT/in-x-$t.fp16" --in 1="$OUT/in-y-$t.fp16")
			;;
		island-b-select-runtime)
			INS=(--in 0="$OUT/in-a-$t.fp16" --in 1="$OUT/in-b-$t.fp16" \
			     --in 2="$OUT/in-c-$t.bin")
			;;
		island-b-select-constfill)
			INS=(--in 0="$OUT/in-a-$t.fp16")
			;;
		rms-c2048-gamma)
			INS=(--in 0="$OUT/in-x-$t.fp16")
			WEIGHTS=$OUT/weights-$t.bin
			;;
		*)
			if [[ $TWO_IN == 1 ]]; then
				INS=(--in 0="$OUT/in-a-$t.fp16" --in 1="$OUT/in-b-$t.fp16")
			else
				INS=(--in 0="$OUT/in-a-$t.fp16")
			fi
			;;
	esac
	if [[ -n $WEIGHTS ]]; then
		if "$RUN" --anec "$ANEC" "${INS[@]}" \
			--out 0="$OUT/out-y-$t.fp16" --check "$CHECK" \
			--weights "$WEIGHTS"; then
			echo "trial $t PASS"
		else
			echo "trial $t FAIL"; fail=1
		fi
	else
		if "$RUN" --anec "$ANEC" "${INS[@]}" \
			--out 0="$OUT/out-y-$t.fp16" --check "$CHECK"; then
			echo "trial $t PASS"
		else
			echo "trial $t FAIL"; fail=1
		fi
	fi
done
# A second process in the same boot closes and reopens the device.
case "$OP" in
	island-a-kt|island-c-pv|island-a-attn-p1|bmm)
		INS=(--in 0="$OUT/in-x-1.fp16" --in 1="$OUT/in-y-1.fp16")
		;;
	island-b-select-runtime)
		INS=(--in 0="$OUT/in-a-1.fp16" --in 1="$OUT/in-b-1.fp16" \
		     --in 2="$OUT/in-c-1.bin")
		;;
	island-b-select-constfill)
		INS=(--in 0="$OUT/in-a-1.fp16")
		;;
	rms-c2048-gamma)
		INS=(--in 0="$OUT/in-x-1.fp16")
		WEIGHTS=$OUT/weights-1.bin
		;;
	*)
		if [[ $TWO_IN == 1 ]]; then
			INS=(--in 0="$OUT/in-a-1.fp16" --in 1="$OUT/in-b-1.fp16")
		else
			INS=(--in 0="$OUT/in-a-1.fp16")
		fi
		;;
esac
if [[ -n $WEIGHTS ]]; then
	if "$RUN" --anec "$ANEC" "${INS[@]}" \
		--out 0="$OUT/out-y-reopen.fp16" --check "$CHECK" --repeat 3 \
		--weights "$WEIGHTS"; then
		echo "reopen+repeat PASS"
	else
		echo "reopen+repeat FAIL"; fail=1
	fi
else
	if "$RUN" --anec "$ANEC" "${INS[@]}" \
		--out 0="$OUT/out-y-reopen.fp16" --check "$CHECK" --repeat 3; then
		echo "reopen+repeat PASS"
	else
		echo "reopen+repeat FAIL"; fail=1
	fi
fi
(cd "$OUT" && sha256sum ./*.fp16 gate.log > SHA256SUMS)
if [[ $fail -eq 0 ]]; then echo "GATE PASS"; else echo "GATE FAIL"; exit 1; fi
