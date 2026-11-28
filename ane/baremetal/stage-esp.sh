#!/bin/sh
# SPDX-License-Identifier: MIT
#
# stage-esp.sh — stage the BMN-03 one-shot trial boot on jwm1's ESP.
#
# Creates:
#   /boot/efi/m1n1/boot-try.bin  = stock stage2 image + bmn03-payload.bin appended
#   /boot/efi/aura-try           = "armed\nm1n1/boot-try.bin\n"
#
# On the next boot, an aura stage1 with trial support rewrites the marker to
# "spent", chainloads boot-try.bin once, and hands the payload the kernel slot
# (it is the last kernel-format record, so it replaces u-boot for that boot
# only). Afterward the stock boot.bin boots normally — nothing needs a manual
# restore. If the stage1 predates aura-try (Oct 30), the marker is ignored and
# still reads "armed" after the boot — the definitive support check.
#
# Run ON jwm1:  sudo ./stage-esp.sh /path/to/bmn03-payload.bin

set -eu
ESP=/boot/efi
PAYLOAD=${1:?usage: $0 <bmn03-payload.bin>}

[ -r "$ESP/m1n1/boot.bin" ] || { echo "no $ESP/m1n1/boot.bin"; exit 1; }

if [ -e "$ESP/aura-try" ]; then
    echo "$ESP/aura-try already exists — refusing to overwrite a pending trial:"
    cat "$ESP/aura-try"
    exit 1
fi

# arm64 Image sanity: "ARM\x64" at +0x38
magic=$(dd if="$PAYLOAD" bs=1 skip=56 count=4 2>/dev/null | od -An -tx1 | tr -d ' \n')
[ "$magic" = "41524d64" ] || { echo "$PAYLOAD: no ARM64 magic at +0x38 ($magic)"; exit 1; }

cp -p "$ESP/m1n1/boot.bin" "$ESP/m1n1/boot-try.bin"
cat "$PAYLOAD" >>"$ESP/m1n1/boot-try.bin"

printf 'armed\nm1n1/boot-try.bin\n' >"$ESP/aura-try"
sync

echo "staged trial: aura-try -> m1n1/boot-try.bin (+$(stat -c %s "$PAYLOAD") bytes appended)"
echo "next boot runs the payload once under the 178 s trial watchdog, then stock boot.bin"
