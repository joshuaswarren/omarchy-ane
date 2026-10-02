#!/bin/bash
# Sustained ANE load for the mid-loop register captures: P6 (compute-bound) and P7 (activation
# stream) alternate in long single-process evaluate runs until $SCRATCH/loopload.stop appears.
# The ANE stays busy across the kext's 2 s poll window.
set -u
S=${SCRATCH:?set SCRATCH}
BIN=$S/ane_inmem_run
[ -x "$BIN" ] || { echo "loopload: $BIN missing"; exit 1; }
rm -f "$S/loopload.stop"
i=0
while [ ! -f "$S/loopload.stop" ]; do
  i=$((i + 1))
  TMPDIR=$S/tmp "$BIN" "$S/p6/model.mil" "$S/p6/weights/weight.bin" "$S/loop-out" 0 2000 \
    "in:$S/p6/in/p6-x.bin:524288" "out:y63:524288" >/dev/null 2>&1
  TMPDIR=$S/tmp "$BIN" "$S/p7/model.mil" "$S/p7/weights/weight.bin" "$S/loop-out" 0 500 \
    "in:$S/p7/in/p7-x.bin:33554432" "in:$S/p7/in/p7-z.bin:33554432" "out:y:33554432" >/dev/null 2>&1
  echo "loop round $i done $(date -u +%FT%TZ)"
done
echo "loopload stopped after $i rounds"
