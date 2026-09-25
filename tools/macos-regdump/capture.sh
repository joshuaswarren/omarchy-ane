#!/bin/zsh
# One-shot T6021 ANE capture on macOS 27. Run from the staged directory.
#
# First load of a user kext needs approval and a reboot. macOS 27 shows the
# kext in System Settings > Privacy & Security > Security, with an "Allow"
# button that stays for about 30 minutes after the load attempt. Click
# Allow, then reboot. After that reboot this script loads the kext and
# captures. Reduced Security with user kexts allowed, and SIP off, are
# prerequisites; the script checks SIP and stops if it is on.
#
# Nothing here reads stdin: every command gets </dev/null, so no step can
# sit at a prompt. Every input file is checked before the first step.
#
# Usage: sudo ./capture.sh <outdir>
emulate -R zsh
setopt errexit nounset pipefail

STAGE=${0:A:h}
OUT=${1:?usage: capture.sh <outdir>}
KEXT=/Library/Extensions/ANERegDump.kext

die() { print -u2 -- "capture.sh: $*"; exit 1; }

for f in ANERegDump.kext/Contents/MacOS/ANERegDump \
	ANERegDump.kext/Contents/Info.plist aneregdump aneprobe \
	aneprobe.mlmodelc/coremldata.bin; do
	[[ -f $STAGE/$f ]] || die "missing $STAGE/$f"
done
[[ $EUID -eq 0 ]] || die "run as root: sudo $0 $OUT"
mkdir -p "$OUT/regdump"

{
	echo "== sw_vers"
	sw_vers
	echo "== csrutil"
	csrutil status
	echo "== boot volume group"
	diskutil info / | grep -i "APFS Volume Group" || echo "(no volume group line)"
} </dev/null | tee "$OUT/host.txt"

csrutil status </dev/null | grep -q 'disabled' || die "SIP is not off"

echo "== kernelcache"
KC=
for f in /System/Volumes/Preboot/*/boot/*/System/Library/Caches/com.apple.kernelcaches/kernelcache(N.); do
	KC=$f
	break
done
[[ -n $KC ]] || die "no kernelcache under /System/Volumes/Preboot"
shasum -a 256 "$KC" </dev/null | tee "$OUT/kernelcache.sha256"

echo "== kmutil load"
# Apple silicon loads user kexts from /Library/Extensions, owned root:wheel.
if ! cmp -s "$STAGE/ANERegDump.kext/Contents/MacOS/ANERegDump" \
	"$KEXT/Contents/MacOS/ANERegDump"; then
	rm -rf "$KEXT"
	cp -R "$STAGE/ANERegDump.kext" "$KEXT"
	chown -R root:wheel "$KEXT"
	chmod -R go-w "$KEXT"
fi
echo "command: sudo kmutil load -p $KEXT"
if ! kmutil load -p "$KEXT" </dev/null 2>"$OUT/kmutil.err"; then
	cat "$OUT/kmutil.err"
	echo
	echo "If the error says the kext needs user approval: open System"
	echo "Settings, Privacy & Security, Security, and click Allow next to"
	echo "ANERegDump. Then reboot. After the reboot, run this script again."
	exit 1
fi
kmutil showloaded --list-only </dev/null 2>/dev/null >"$OUT/kext-loaded.txt" || true
grep -qi ANERegDump "$OUT/kext-loaded.txt" ||
	die "kmutil load returned 0 but ANERegDump is not in kmutil showloaded"
grep -i ANERegDump "$OUT/kext-loaded.txt"

echo "== ioreg before"
ioreg -lw0 -c H11ANEIn </dev/null > "$OUT/ioreg-before.txt"

echo "== workload + log stream + powermetrics"
log stream --level debug --predicate 'subsystem CONTAINS "ane" OR process == "aned"' \
	</dev/null > "$OUT/log-stream.txt" 2>&1 &
LOGPID=$!
powermetrics --samplers ane_power -i 500 -n 40 \
	</dev/null > "$OUT/powermetrics.txt" 2>&1 &
PMPID=$!
# The kext polls the power gate for 2 s inside the dump. The workload
# has to be running for that whole poll, not started after it.
"$STAGE/aneprobe" "$STAGE/aneprobe.mlmodelc" 25 \
	</dev/null > "$OUT/workload.txt" 2>&1 &
WLPID=$!
sleep 0.2
echo "== regdump"
[[ -f $STAGE/ranges.txt ]] || die "missing $STAGE/ranges.txt"
echo "request: $STAGE/ranges.txt"
RC=0
"$STAGE/aneregdump" "$OUT/regdump" "$STAGE/ranges.txt" </dev/null || RC=$?
wait $WLPID || true
kill $LOGPID $PMPID 2>/dev/null || true
wait $LOGPID $PMPID 2>/dev/null || true

echo "== ioreg after"
ioreg -lw0 -c H11ANEIn </dev/null > "$OUT/ioreg-after.txt"

echo "== sums"
cd "$OUT"
files=( **/*(.N) )
shasum -a 256 $files </dev/null > SHA256SUMS
cat SHA256SUMS
echo "aneregdump exit=$RC (0 = islands up, 3 = islands gated)"
exit $RC
