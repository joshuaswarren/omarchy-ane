#!/bin/zsh
# One-shot T6021 ANE capture on macOS 27. Run from the staged directory.
#
# First load of a user kext needs approval and a reboot. macOS 27 shows the
# kext in System Settings > Privacy & Security > Security, with an "Allow"
# button that stays for about 30 minutes after the load attempt. Click
# Allow, then reboot. After that reboot this script loads the kext and
# captures. Reduced Security with user kexts allowed, and SIP off, are
# prerequisites; the script checks both and stops if either is wrong.
#
# Usage: sudo ./capture.sh <outdir>
set -euo pipefail

STAGE=${0:A:h}
OUT=${1:?usage: capture.sh <outdir>}
mkdir -p "$OUT"

{
	echo "== sw_vers"
	sw_vers
	echo "== csrutil"
	csrutil status
	echo "== boot volume group"
	diskutil info / | grep -i "APFS Volume Group"
} | tee "$OUT/host.txt"

csrutil status | grep -q 'disabled' || { echo "SIP is not off"; exit 1; }

echo "== kernelcache"
KC=$(ls /System/Volumes/Preboot/*/boot/*/System/Library/Caches/com.apple.kernelcaches/kernelcache 2>/dev/null | head -1)
[[ -n "$KC" ]] || { echo "no kernelcache under Preboot"; exit 1; }
shasum -a 256 "$KC" | tee "$OUT/kernelcache.sha256"

echo "== kmutil load"
echo "command: sudo kmutil load -p $STAGE/ANERegDump.kext"
if ! kmutil load -p "$STAGE/ANERegDump.kext" 2>"$OUT/kmutil.err"; then
	cat "$OUT/kmutil.err"
	echo
	echo "If the error says the kext needs user approval: open System"
	echo "Settings, Privacy & Security, Security, and click Allow next to"
	echo "ANERegDump. Then reboot. After the reboot, run this script again."
	exit 1
fi
kmutil showloaded --list-only 2>/dev/null | grep -i ANERegDump | tee "$OUT/kext-loaded.txt"

echo "== ioreg before"
ioreg -lw0 -c H11ANEIn > "$OUT/ioreg-before.txt"

echo "== workload + log stream + powermetrics"
log stream --level debug --predicate 'subsystem CONTAINS "ane" OR process == "aned"' \
	> "$OUT/log-stream.txt" &
LOGPID=$!
powermetrics --samplers ane_power -i 1000 -n 20 > "$OUT/powermetrics.txt" &
PMPID=$!
"$STAGE/aneprobe" "$STAGE/aneprobe.mlmodelc" 15 > "$OUT/workload.txt" &
WLPID=$!

sleep 5
echo "== regdump"
set +e
"$STAGE/aneregdump" "$OUT/regdump"
RC=$?
set -e
wait $WLPID || true
kill $LOGPID $PMPID 2>/dev/null || true
wait $LOGPID $PMPID 2>/dev/null || true

echo "== ioreg after"
ioreg -lw0 -c H11ANEIn > "$OUT/ioreg-after.txt"

echo "== sums"
(cd "$OUT" && shasum -a 256 **/*(N) *(.N) | tee SHA256SUMS)
echo "aneregdump exit=$RC (0 = islands up, 3 = islands gated)"
exit $RC
