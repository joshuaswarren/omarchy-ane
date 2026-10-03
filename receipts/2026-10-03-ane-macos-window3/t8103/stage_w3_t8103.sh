#!/bin/bash
# Build the T8103 window-3 stage on the operator host: window-1 prepare_stage.py (P6, P7, P6',
# the pinned ANE runner; no encoder) plus w3gen.py, w2sample.py, the T8103 phase script and the cold
# encoder runner source (binary too when given). The encoder .mlpackage and inputs are NOT staged: they
# already live on the laptop's macOS in the mac-reference-bundle (window 4), passed as MAC_BUNDLE.
#   E1=<e1-probes> P6FIX=<P6Fix e1-p6overfix> INMEM_BIN=<ane_inmem_run 2bbc237b> [ENC_BIN=<encoder_ramp>] \
#   OUT=<new stage dir> bash stage_w3_t8103.sh
set -euo pipefail
E1=${E1:?} P6FIX=${P6FIX:?} INMEM_BIN=${INMEM_BIN:?} OUT=${OUT:?}
R=$(cd "$(dirname "$0")/.." && pwd)
REPO=$(cd "$R/../.." && pwd)
G=$REPO/receipts/2026-10-02-ane-macos-gap-window/macos-bundle
W2=$REPO/receipts/2026-10-03-ane-macos-window2/macos-bundle
INMEM_SHA=2bbc237bd505cfebccd8f40aaca854ac0a7e6fbea6e48da0ba73df8b9d7a27ef
[ "$(sha256sum "$INMEM_BIN" | cut -d' ' -f1)" = "$INMEM_SHA" ] || { echo "INMEM_BIN is not 2bbc237b"; exit 1; }
T=$(mktemp -d)
trap 'python3 -c "import shutil, sys; shutil.rmtree(sys.argv[1])" "$T"' EXIT
mkdir -p "$T/p6prime/weights" "$T/p6prime/in"
cp "$P6FIX/p6prime-model.mil" "$T/p6prime/model.mil"
cp "$P6FIX/p6prime-weight.bin" "$T/p6prime/weights/weight.bin"
cp "$P6FIX/p6prime-x.npy" "$T/p6prime/in/x.npy"
cp "$P6FIX/p6prime-golden.npy" "$T/p6prime/in/golden.npy"
cp "$P6FIX/p6prime-manifest.json" "$T/p6prime/manifest.json"
python3 "$G/prepare_stage.py" --e1 "$E1" --e1-prime "$T" --inmem-src "$REPO/tools/native-macos/ane_inmem_run.m" \
	--inmem-bin "$INMEM_BIN" --out "$OUT"
cp "$R/macos-bundle/w3gen.py" "$W2/w2sample.py" "$R/t8103/macos_window3_t8103.sh" "$R/t8103/encoder_ramp.swift" "$OUT/"
list="macos_window.sh w3gen.py w2sample.py macos_window3_t8103.sh encoder_ramp.swift"
if [ -n "${ENC_BIN:-}" ]; then cp "$ENC_BIN" "$OUT/encoder_ramp"; chmod 755 "$OUT/encoder_ramp"; list="$list encoder_ramp"; fi
# shellcheck disable=SC2086
(cd "$OUT" && sha256sum $list >>stage_manifest.sha256 && sha256sum -c stage_manifest.sha256 --quiet)
echo "stage $OUT: $(wc -l <"$OUT/stage_manifest.sha256") manifest entries, $(du -sh "$OUT" | cut -f1)"
