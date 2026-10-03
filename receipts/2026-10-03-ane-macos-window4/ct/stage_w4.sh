#!/bin/bash
# Build the window-4 macOS stage on the operator host (no M2 contact): the window-3 stage (P6/P7/P6', the
# ANE runner, macos_window.sh, macos_window3.sh, w3gen.py, w2sample.py) plus macos_window2.sh (its base
# and w2env phases are reused), the window-4 phase script, ranges-w4.txt and the Studio-built CLI with the
# -n check mode. Every executed file is in stage_manifest.sha256.
#   E1=<e1-probes> P6FIX=<P6Fix e1-p6overfix> INMEM_BIN=<ane_inmem_run 2bbc237b> \
#   REGDUMP_BIN=<aneregdump built from this commit> OUT=<new stage dir> bash stage_w4.sh
set -euo pipefail
E1=${E1:?} P6FIX=${P6FIX:?} INMEM_BIN=${INMEM_BIN:?} REGDUMP_BIN=${REGDUMP_BIN:?} OUT=${OUT:?}
R=$(cd "$(dirname "$0")/.." && pwd)
REPO=$(cd "$R/../.." && pwd)
W2=$REPO/receipts/2026-10-03-ane-macos-window2/macos-bundle
W3=$REPO/receipts/2026-10-03-ane-macos-window3
E1=$E1 P6FIX=$P6FIX INMEM_BIN=$INMEM_BIN OUT=$OUT bash "$W3/ct/stage_w3.sh"
cp "$W2/macos_window2.sh" "$W2/ranges-w2.txt" "$R/macos-bundle/macos_window4.sh" "$R/macos-bundle/ranges-w4.txt" "$OUT/"
mkdir -p "$OUT/regdump-build"
cp "$REGDUMP_BIN" "$OUT/regdump-build/aneregdump"
chmod 755 "$OUT/regdump-build/aneregdump"
cp "$REPO/tools/macos-regdump/aneregdump.c" "$REPO/tools/macos-regdump/ane_regdump_filter.h" "$OUT/src/macos-regdump/"
(cd "$OUT" && sha256sum macos_window2.sh ranges-w2.txt macos_window4.sh ranges-w4.txt regdump-build/aneregdump \
	src/macos-regdump/aneregdump.c src/macos-regdump/ane_regdump_filter.h >>stage_manifest.sha256 &&
	sha256sum -c stage_manifest.sha256 --quiet)
echo "stage $OUT: $(wc -l <"$OUT/stage_manifest.sha256") manifest entries, $(du -sh "$OUT" | cut -f1)"
