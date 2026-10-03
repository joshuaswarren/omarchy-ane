#!/bin/bash
set -euo pipefail
# collect-macos.sh: read-only ANE facts from macOS on a T8112 Mac, for the omarchy-ane lab.
#
#   bash collect-macos.sh     # writes ./t8112-macos-<model>-<UTC>.tar.gz
#
# No sudo, no network, no secrets. It saves the IODeviceTree ANE, ANE DART and
# pmgr nodes (ioreg -a), four /arm-io and four /chosen properties, sw_vers,
# sysctl hw.model, and the model and chip lines of system_profiler (not the
# serial number or the UUIDs), with SHA256SUMS. ingest.py --macos reads it.

[[ $(uname -s) == Darwin ]] || { echo "collect-macos.sh: run this on macOS" >&2; exit 1; }
start=$PWD
name=t8112-macos-$(sysctl -n hw.model)-$(date -u +%Y%m%dT%H%M%SZ)
tmp=$(mktemp -d)
mkdir "$tmp/$name"
cd "$tmp/$name"

# node FILE NAME...: the first IODeviceTree subtree that has one of the names
node() {
  local out=$1 n
  shift
  for n; do
    ioreg -a -p IODeviceTree -r -n "$n" > "$out"
    grep -q '<dict>' "$out" && return 0
  done
  echo "collect-macos.sh: no IODeviceTree node named $*" >&2
  exit 1
}

sw_vers > sw_vers.txt
sysctl hw.model machdep.cpu.brand_string > hw.model.txt
system_profiler SPHardwareDataType |
  grep -E '^ *(Model Name|Model Identifier|Chip|Total Number of Cores|Memory|System Firmware Version|OS Loader Version):' \
  > hardware.txt
node ioreg-ane.plist ane ane0
node ioreg-dart-ane.plist dart-ane dart-ane0
node ioreg-pmgr.plist pmgr
ioreg -p IODeviceTree -r -n arm-io -d 1 |
  grep -E '"(compatible|chip-revision|fuse-revision|soc-generation)" =' > arm-io.txt
ioreg -p IODeviceTree -r -n chosen -d 1 |
  grep -E '"(chip-id|board-id|firmware-version|system-firmware-version)" =' > chosen.txt
shasum -a 256 -- * > SHA256SUMS

cd "$start"
tar -C "$tmp" -czf "$name.tar.gz" "$name"
rm -rf "$tmp"
shasum -a 256 "$start/$name.tar.gz"
echo "Send $start/$name.tar.gz to the lab."
