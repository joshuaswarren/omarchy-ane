#!/bin/sh
# SPDX-License-Identifier: MIT
#
# stage-bootnext.sh — stage/unstage the BMN-05 EFI-app one-shot on a lab Mac.
#
# The 2026-11-27 boot loop came from `efibootmgr -c` alone: it inserts the
# new entry FIRST in BootOrder, so the app ran on every boot, re-armed the
# watchdog, and Linux never loaded. This script is the corrected staging:
#
#   1. Save the exact pre-change BootOrder to /root/bmn-bootorder.txt.
#   2. Create the entry, then IMMEDIATELY restore the saved order — our
#      entry stays out of BootOrder entirely.
#   3. Set BootNext to our entry: the boot manager tries it exactly once,
#      consumes BootNext, and the untouched order boots Linux. A manager
#      that ignores BootNext simply boots Linux — every failure mode is stock.
#
# `unstage` restores the saved order, deletes the entry, the .efi file and
# the BMN03S/BMN03R variables, and prints the final listing.
#
# Run ON the target Mac:  sudo ./stage-bootnext.sh stage /path/to/app.efi [label]
#                         sudo ./stage-bootnext.sh unstage [entry-number]

set -eu
ESP=/boot/efi
STATE=/root/bmn-bootorder.txt
CMD=${1:?usage: $0 stage <app.efi> [label] | unstage [entry]}

case "$CMD" in
stage)
	EFI=${2:?missing app.efi path}
	LABEL=${3:-BMN05}
	[ -r "$EFI" ] || { echo "$EFI: not readable"; exit 1; }
	head -c2 "$EFI" | grep -q MZ || { echo "$EFI: not a PE image"; exit 1; }

	# 1. Exact pre-state for restore.
	efibootmgr -v >"$STATE.efibootmgr"
	ORDER=$(efibootmgr | sed -n 's/^BootOrder: //p')
	printf '%s\n' "$ORDER" >"$STATE"
	echo "saved BootOrder: ${ORDER:-<empty>}"

	# 2. Create the entry — -c puts it first, so restore the order at once.
	DEV=$(findmnt -n -o SOURCE "$ESP")
	DISK=$(lsblk -n -o PKNAME "$DEV" | head -1)
	PART=$(echo "$DEV" | grep -oE '[0-9]+$')
	efibootmgr -c -d "/dev/$DISK" -p "$PART" -L "$LABEL" -l "\\EFI\\Linux\\$(basename "$EFI")"
	NEW=$(efibootmgr | awk -v l="$LABEL" '$0 ~ ("Boot[0-9]+.*" l) {gsub("Boot","",$1); gsub("\\*","",$1); print $1}' | head -1)
	[ -n "$NEW" ] || { echo "entry for $LABEL not found after -c"; exit 1; }
	cp -p "$EFI" "$ESP/EFI/Linux/$(basename "$EFI")"

	# 3. Order back exactly as saved; then the one-shot pointer.
	[ -n "$ORDER" ] && efibootmgr -o "$ORDER"
	efibootmgr -n "$NEW"
	sync
	echo "staged: BootNext=$NEW -> \\EFI\\Linux\\$(basename "$EFI"); BootOrder restored to: $ORDER"
	echo "next boot runs the app once; every path after it is stock."
	efibootmgr -v
	;;

unstage)
	ENTRY=${2:-}
	[ -n "$ENTRY" ] || ENTRY=$(efibootmgr | sed -n 's/^Boot\([0-9A-Fa-f]\{4\}\)\*.*BMN.*/\1/p' | head -1)
	[ -f "$STATE" ] && efibootmgr -o "$(cat "$STATE")"
	[ -n "$ENTRY" ] && efibootmgr -b "$ENTRY" -B
	rm -f "$ESP/EFI/Linux"/bmn*.efi "$ESP/EFI/Linux"/kex*.efi
	efivar -n 8be4df61-93ca-11d2-aa0d-00e098032b8c-BMN03S -d 2>/dev/null || true
	efivar -n 8be4df61-93ca-11d2-aa0d-00e098032b8c-BMN03R -d 2>/dev/null || true
	sync
	echo "unstaged; final listing:"
	efibootmgr -v
	;;

*)
	echo "usage: $0 stage <app.efi> [label] | unstage [entry]" >&2
	exit 1
	;;
esac
