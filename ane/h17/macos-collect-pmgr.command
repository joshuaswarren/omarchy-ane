#!/bin/bash
# Read-only macOS evidence collection. No firmware/boot/security changes.
set -eu
umask 077
if [ "$(uname -s)" != Darwin ]; then
  echo 'Run this script on the J700 Mac under macOS.' >&2
  exit 1
fi
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=$(mktemp -d "$base/macos-pmgr-$(date -u +%Y%m%dT%H%M%SZ)-XXXXXX")
capture() {
  name=$1
  shift
  if "$@" >"$out/$name" 2>"$out/$name.stderr"; then
    echo "$name: OK" >>"$out/status.txt"
  else
    rc=$?
    echo "$name: FAILED ($rc); see stderr" >>"$out/status.txt"
  fi
}
capture os.txt /usr/bin/sw_vers
capture kernel.txt /usr/bin/uname -a
capture model.txt /usr/sbin/sysctl hw.model hw.machine hw.memsize
capture device-tree.plist /usr/sbin/ioreg -a -l -p IODeviceTree
capture service-tree.plist /usr/sbin/ioreg -a -l -p IOService
capture device-tree.txt /usr/sbin/ioreg -l -w 0 -p IODeviceTree
capture pmgr-services.txt /usr/sbin/ioreg -l -w 0 -r -c ApplePMGR
capture ane-services.txt /usr/sbin/ioreg -l -w 0 -r -c H11ANEIn
capture security-status.txt /usr/bin/csrutil status
cat >"$out/READ-ME.txt" <<'EOF'
Return this entire directory for offline analysis.
The registry dumps can contain serial numbers and identifiers: keep them private.

This collector does NOT read kernel heap memory or physical registers.
RegMap/RegGroup tables may not be exported in IORegistry. Their absence here
does not mean they do not exist. An exact mapping may still require a separate
kernel-debugging setup with matching symbols and a verified ApplePMGR object.
The offsets +0x29e8 and +0x60f8 refer to candidate object fields from one
retained kernel build, NOT physical PMGR addresses. Do not dereference them
without verifying the running build, object type, layout and pointer semantics.
Do not reuse T602x (M2) addresses for T8140.
EOF
(
  cd "$out"
  /usr/bin/shasum -a 256 ./*.txt ./*.plist ./*.stderr > SHA256SUMS
)
/bin/sync
echo "Saved and synced: $out"
echo 'Bring the SD card back with this directory intact.'
