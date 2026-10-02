#!/bin/bash
# Build the window-2 macOS stage on the operator host (no M2 contact): window-1 prepare_stage.py
# (P6, P7, P6', the inmem runner; no encoder) plus the window-2 files, every executed file in
# stage_manifest.sha256 so the on-box `inputs` check covers it.
#   E1=<e1-probes with p6/ p7/> P6FIX=<P6Fix e1-p6overfix dir> INMEM_BIN=<ane_inmem_run 2bbc237b> \
#   REGDUMP_BIN=<aneregdump with the series mode, Studio build> OUT=<new stage dir> bash stage_w2.sh
set -euo pipefail
E1=${E1:?} P6FIX=${P6FIX:?} INMEM_BIN=${INMEM_BIN:?} REGDUMP_BIN=${REGDUMP_BIN:?} OUT=${OUT:?}
R=$(cd "$(dirname "$0")/.." && pwd)
REPO=$(cd "$R/../.." && pwd)
G=$REPO/receipts/2026-10-02-ane-macos-gap-window/macos-bundle
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
	--inmem-bin "$INMEM_BIN" --regdump-src "$REPO/tools/macos-regdump" --out "$OUT"
cp "$R/macos-bundle/macos_window2.sh" "$R/macos-bundle/w2sample.py" "$R/macos-bundle/ranges-w2.txt" "$OUT/"
mkdir -p "$OUT/regdump-build"
cp "$REGDUMP_BIN" "$OUT/regdump-build/aneregdump"
chmod 755 "$OUT/regdump-build/aneregdump"
(cd "$OUT" && sha256sum macos_window.sh loopload.sh macos_window2.sh w2sample.py ranges-w2.txt \
	regdump-build/aneregdump >>stage_manifest.sha256 && sha256sum -c stage_manifest.sha256 --quiet)
echo "stage $OUT: $(wc -l <"$OUT/stage_manifest.sha256") manifest entries, $(du -sh "$OUT" | cut -f1)"
