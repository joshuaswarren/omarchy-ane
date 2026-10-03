#!/bin/bash
set -euo pipefail
# Stage the native run on a Mac. Run on the analysis host. TARGET and SOURCE are ssh aliases: TARGET is the
# Mac under test, SOURCE the Mac that holds the M1 reference runtime. Large inputs stream host to host and are
# never staged on the analysis host:
#   SOURCE -> TARGET: Python 3.12.11 (uv), the venv's site-packages (numpy 2.5.2, gguf 0.19.0), ane-compile-hwx,
#                     ANEForge 2ea941c (git archive) + its built e5rt dispatch dylib, the contract GGUF,
#                     ane_inmem_run (built on SOURCE from ane_inmem_run.m, macOS 14+ deployment target).
#   this host -> TARGET: these tools, chunk_00.json, manifest.json, Studio h14 fingerprints, Parakeet MIL,
#                        weight.bin (444 MB) and the fp16 fixture inputs.
#   TARGET=<m2> SOURCE=<studio> SOURCE_GGUF=<path> SOURCE_BUILD=<dir> SCRATCH=<dir on TARGET> PREP=<dir> \
#     PK_MIL=<model.mil> PK_W=<weight.bin> bash transfer.sh
: "${TARGET:?}" "${SOURCE:?}" "${SOURCE_GGUF:?}" "${SOURCE_BUILD:?}" "${SCRATCH:?}" "${PREP:?}" "${PK_MIL:?}" "${PK_W:?}"
here=$(cd "$(dirname "$0")" && pwd)
tools=$(cd "$here/.." && pwd)
S=$SCRATCH B=$SOURCE_BUILD GGUF=$SOURCE_GGUF
AF=src/ane-af-split-wt AF_COMMIT=2ea941ce7ec58a802de792c152726c3a9a881706

ssh "$TARGET" "mkdir -p $S/tools/native-macos $S/tools/staged-qwen $S/bin $S/gguf $S/parakeet/weights $S/parakeet/in $S/aneforge-src"

ssh "$SOURCE" "COPYFILE_DISABLE=1 tar cf - -C ~/.local/share/uv/python cpython-3.12.11-macos-aarch64-none \
  -C ~/ane-venv/lib/python3.12 site-packages -C ~/recurrent-mint ane-compile-hwx" \
  | ssh "$TARGET" "tar xf - -C $S -s ',^cpython-3.12.11-macos-aarch64-none,py,' -s ',^ane-compile-hwx\$,bin/ane-compile-hwx,'"
ssh "$SOURCE" "cd $AF && git archive $AF_COMMIT aneforge pyproject.toml" | ssh "$TARGET" "tar xf - -C $S/aneforge-src"
ssh "$SOURCE" "cd $AF && COPYFILE_DISABLE=1 tar cf - aneforge/_lib/libane_e5rt_dispatch.dylib" | ssh "$TARGET" "tar xf - -C $S/aneforge-src"
ssh "$SOURCE" "mkdir -p $B && cat > $B/ane_inmem_run.m" < "$here/ane_inmem_run.m"
ssh "$SOURCE" "xcrun clang -O2 -Wall -fobjc-arc -mmacosx-version-min=14.0 -framework Foundation -framework IOSurface \
  $B/ane_inmem_run.m -o $B/ane_inmem_run && cat $B/ane_inmem_run" | ssh "$TARGET" "cat > $S/bin/ane_inmem_run && chmod +x $S/bin/ane_inmem_run"
echo "ANEForge lane/deltanet-split-decode $AF_COMMIT (git archive) + dylib built on the source Mac" \
  | ssh "$TARGET" "cat > $S/aneforge-src/COMMIT"

size=$(ssh "$SOURCE" "stat -f %z $GGUF")
if [ "$(ssh "$TARGET" "stat -f %z $S/gguf/Qwen3.8-2B-Q4_K_M.gguf 2>/dev/null || echo 0")" != "$size" ]; then
  ssh "$SOURCE" "cat $GGUF" | ssh "$TARGET" "cat > $S/gguf/Qwen3.8-2B-Q4_K_M.gguf"
fi

tar cf - -C "$tools" native-macos staged-qwen/dump_step_ports.py hwx_h14_staged_to_anec.py | ssh "$TARGET" "tar xf - -C $S/tools"
tar cf - -C "$PREP" chunk_00.json manifest.json studio-h14-fingerprints.jsonl studio-h14-parakeet-fingerprint.jsonl \
  | ssh "$TARGET" "tar xf - -C $S/tools/native-macos"
tar cf - -C "$PREP/parakeet-in" input_features.npy attention_mask.npy | ssh "$TARGET" "tar xf - -C $S/parakeet/in"
ssh "$TARGET" "cat > $S/parakeet/model.mil" < "$PK_MIL"
if [ "$(ssh "$TARGET" "stat -f %z $S/parakeet/weights/weight.bin 2>/dev/null || echo 0")" != "$(stat -c %s "$PK_W")" ]; then
  ssh "$TARGET" "cat > $S/parakeet/weights/weight.bin" < "$PK_W"
fi
ssh "$TARGET" "cd $S && shasum -a 256 gguf/*.gguf parakeet/model.mil parakeet/weights/weight.bin parakeet/in/*.npy \
  bin/ane-compile-hwx bin/ane_inmem_run aneforge-src/aneforge/_lib/libane_e5rt_dispatch.dylib tools/native-macos/*.json* && du -sh $S"
