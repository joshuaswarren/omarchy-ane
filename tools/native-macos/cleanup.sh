#!/bin/bash
# Remove a native run's scratch after its results were copied back. Only the two run directories qualify.
#   SCRATCH=$HOME/oracle-mint-scratch/native-run bash cleanup.sh
set -euo pipefail
S=${SCRATCH:?}
case $S in
  */oracle-mint-scratch/native-run | */native-run-dryrun) ;;
  *) echo "refusing to remove $S"; exit 2 ;;
esac
CAP=${HWX_CAPTURE_ROOT:-/tmp/qwen-real-full-1790790164}
rm -rf "$S"
for d in "$CAP/captures" "$CAP/out" "$CAP"; do
  if [ -d "$d" ]; then rmdir "$d"; fi
done
df -h /System/Volumes/Data
ls -la ~/.cache/aneforge ~/Models/.aneforge-cache 2>&1 | head -5 || true
