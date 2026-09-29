#!/bin/bash
# Installed-path gate for the T6021 ANE: checks the install, then runs the
# add program through libane (ane-run) with seeded random inputs and checks
# y == a + b under fp16 half-away-from-zero rounding.
#
# usage: gate.sh OUTDIR [--insmod KO]
#   --insmod KO  load KO with no parameters first (for a boot where the
#                module is installed but not yet in modules.dep).
# Run from a fresh boot. The module cannot be unloaded, so this never rmmods.
set -euo pipefail
OUT=${1:?output dir required}
KO=""
[[ ${2:-} == --insmod ]] && KO=${3:?module path required}
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
FW=/lib/firmware/apple/ane/t602x_ane0_fw_selene_rc4x.macho
FW_SHA=a9c4b771294a6b115624d9480a6248d0899a1681a575e865070b87a3248427bc
ANEC=$ROOT/fixtures/program-0.anec
RUN=$ROOT/tools/ane-run
TRIALS=${TRIALS:-4}
mkdir -p "$OUT"
exec > >(tee "$OUT/gate.log") 2>&1

echo "boot_id $(cat /proc/sys/kernel/random/boot_id)"
echo "kernel $(uname -r)"
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
	python3 - "$OUT" "$t" <<'PY'
# The add fixture is nchw [1,512,1,1] with a 64-byte plane stride: 512
# valid fp16 lanes, one per 64 bytes (index % 32 == 0). Padding lanes must
# be zero because the ANE ignores them and writes zero.
import numpy as np, sys
out, t = sys.argv[1], int(sys.argv[2])
rng = np.random.default_rng(1000 + t)
for name in ("a", "b"):
    x = np.zeros(16384, dtype=np.float16)
    x[::32] = rng.uniform(-8, 8, 512).astype(np.float16)
    x.tofile(f"{out}/in-{name}-{t}.fp16")
PY
	if "$RUN" --anec "$ANEC" --in 0="$OUT/in-a-$t.fp16" --in 1="$OUT/in-b-$t.fp16" \
		--out 0="$OUT/out-y-$t.fp16" --check-add; then
		echo "trial $t PASS"
	else
		echo "trial $t FAIL"; fail=1
	fi
done
# A second process in the same boot closes and reopens the device.
if "$RUN" --anec "$ANEC" --in 0="$OUT/in-a-1.fp16" --in 1="$OUT/in-b-1.fp16" \
	--out 0="$OUT/out-y-reopen.fp16" --check-add --repeat 3; then
	echo "reopen+repeat PASS"
else
	echo "reopen+repeat FAIL"; fail=1
fi
(cd "$OUT" && sha256sum ./*.fp16 gate.log > SHA256SUMS)
if [[ $fail -eq 0 ]]; then echo "GATE PASS"; else echo "GATE FAIL"; exit 1; fi
