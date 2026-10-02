#!/bin/bash
# shellcheck disable=SC2024 # sudo output goes to the caller's own files in STAGEDIR
# Install the in-tree ANE kernel next to the stock one: module tree, /boot/vmlinuz-ane-intree,
# /boot/initramfs-ane-intree.img and /boot/grub/custom.cfg (two one-shot entries). Writes only new
# paths and proves every stock boot file unchanged. Derived from the DartKernel install.sh/modules.sh
# (receipts/2026-10-01-t6021-dart-kernel/scripts); the GRUB entries are generated from this host's
# stock /proc/cmdline and /boot UUID, so the same script serves the M2 Max and the M1 Max.
# Run from the stock kernel, default boot (no one-shot marker).
# usage: stage.sh STAGEDIR            full install (modules, kernel, initramfs, custom.cfg)
#        stage.sh STAGEDIR modules    module tree only: run it right before the in-tree reboot, because
#                                     linux-modules-cleanup.service (kernel-modules-hook) moves every
#                                     unowned tree that is not the running kernel's to /usr/lib/modules/.old
#                                     at each boot, and tmpfiles empties .old at the next one.
set -euo pipefail
trap 'echo "FAIL line $LINENO: $BASH_COMMAND"' ERR
S=$(realpath "${1:?stagedir}")
MODE=${2:-install}
REL=$(cat "$S/release")
STOCK=$(uname -r)
M=/usr/lib/modules
SK=/boot/vmlinuz-linux-asahi
SI=/boot/initramfs-linux-asahi.img
K=/boot/vmlinuz-ane-intree
I=/boot/initramfs-ane-intree.img
C=/boot/grub/custom.cfg
VM="$REL SMP preempt mod_unload aarch64"
cd "$S"

stock_hashes() {
	local f
	for f in "$SK" "$SI" /boot/grub/grub.cfg /boot/grub/grubenv /boot/efi/EFI/BOOT/BOOTAA64.EFI /boot/efi/m1n1/boot.bin; do
		if sudo -n test -e "$f"; then sudo -n sha256sum "$f"; else echo "absent  $f"; fi
	done
	echo "$(cd "$M/$STOCK" && find . -type f -print0 | sort -z | xargs -0 sha256sum | sha256sum | cut -c1-64)  stock-module-tree"
}

modules() {
	if [ -e "$M/$REL" ]; then
		sudo -n tar -d -C "$M/$REL" -f modules.tar
	else
		sudo -n mkdir "$M/$REL"
		sudo -n tar -x --no-same-owner -C "$M/$REL" -f modules.tar
		sudo -n tar -d -C "$M/$REL" -f modules.tar
	fi
	[ ! -e "$M/$REL/updates" ] || { echo "FAIL: $M/$REL/updates exists (an out-of-tree module would win)"; exit 2; }
	sudo -n depmod -a "$REL"
	for m in ane ane_t6021; do
		[ "$(realpath "$(modinfo -k "$REL" -n "$m")")" = "$M/$REL/kernel/drivers/accel/ane/$m.ko" ]
		[ "$(modinfo -k "$REL" -F intree "$m")" = Y ]
	done
	(cd "$M/$REL" && sha256sum -c --quiet "$S/ane-modules.sha256")
	for m in ane ane_t6021 brcmfmac zram tun btrfs; do
		[ "$(modinfo -k "$REL" -F vermagic "$m" | xargs)" = "$VM" ]
	done
	sync
	echo "MODULES OK $(date -u +%FT%TZ): $M/$REL, $(find "$M/$REL" -name '*.ko' | wc -l) modules, ane and ane_t6021 in-tree"
}

[ "$REL" != "$STOCK" ] || { echo "FAIL: running $REL, run from the stock kernel"; exit 2; }
case "$(cat /proc/cmdline)" in
"BOOT_IMAGE=/vmlinuz-linux-asahi "*oneshot=*) echo "FAIL: one-shot boot, run from the default boot"; exit 2 ;;
"BOOT_IMAGE=/vmlinuz-linux-asahi "*) ;;
*) echo "FAIL: not the stock default boot: $(cat /proc/cmdline)"; exit 2 ;;
esac
sha256sum -c --quiet SHA256SUMS
if [ "$MODE" = modules ]; then
	modules
	exit 0
fi
[ "$MODE" = install ] || { echo "bad mode $MODE"; exit 2; }

for f in "$M/$REL" "$K" "$I" "$C"; do
	[ ! -e "$f" ] || { echo "FAIL: exists: $f (revert.sh first)"; exit 2; }
done
[ -z "$(sudo -n grub-editenv /boot/grub/grubenv list | sed -n 's/^next_entry=//p')" ] || { echo "FAIL: a one-shot is pending"; exit 2; }
sudo -n grep -q 'custom.cfg' /boot/grub/grub.cfg || { echo "FAIL: grub.cfg does not source custom.cfg"; exit 2; }
[ "$(findmnt -no TARGET /boot)" = /boot ] && [ "$(findmnt -no FSTYPE /boot)" = ext4 ]
stock_hashes | tee pre-stock.txt

modules

sudo -n install -m644 Image "$K"
sudo -n mkinitcpio -k "$REL" -g "$I" >mkinitcpio.log 2>&1
sudo -n lsinitcpio "$I" >initramfs.list
grep -q "^usr/lib/modules/$REL/" initramfs.list
if grep -q "^usr/lib/modules/$STOCK/" initramfs.list; then echo "FAIL: stock modules in the image"; exit 2; fi
# Reference only (the trees differ: aurora 7.1.12 vs the stock package): a stock-kernel image built now.
sudo -n mkinitcpio -k "$STOCK" -g "$S/stock-ref.img" >stock-ref.log 2>&1
diff <(sudo -n lsinitcpio "$S/stock-ref.img" | sed "s|/$STOCK/|/KVER/|" | sort) \
	<(sed "s|/$REL/|/KVER/|" initramfs.list | sort) >initramfs-vs-stock.diff || true
echo "initramfs: $(wc -l <initramfs.list) files, $(grep -c '^[<>]' initramfs-vs-stock.diff || true) differ from a stock image built now (initramfs-vs-stock.diff)"

# Both entries: the stock command line of this boot plus the marker, panic=30 and the initrd watchdog.
args="$(sed 's/^BOOT_IMAGE=[^ ]* //' /proc/cmdline) panic=30 systemd.watchdog_sec=120 systemd.crash_action=reboot"
uuid=$(findmnt -no UUID /boot)
entry() { # title id kernel initrd marker
	printf "menuentry '%s' --class os --id %s {\n" "$1" "$2"
	printf '\tload_video\n\tset gfxpayload=keep\n\tinsmod gzio\n\tinsmod part_gpt\n\tinsmod ext2\n'
	printf '\tsearch --no-floppy --fs-uuid --set=root %s\n' "$uuid"
	printf '\tlinux\t%s %s ane_intree_oneshot=%s\n' "$3" "$args" "$5"
	printf '\tinitrd\t%s\n}\n' "$4"
}
{
	echo "# ane-intree one-shot entries (omarchy-ane receipts/2026-10-02-ane-intree-boot). Only grub-reboot"
	echo "# selects them; the default stays the stock entry of grub.cfg."
	entry "ane-intree T: stock kernel, one-shot test" ane-intree-oneshot-test "${SK#/boot}" "${SI#/boot}" test
	entry "ane-intree I: $REL" ane-intree "${K#/boot}" "${I#/boot}" intree
} >custom.cfg
sudo -n install -m644 custom.cfg "$C"
sudo -n grub-script-check "$C"
sync

sudo -n sha256sum "$K" "$I" "$C" | tee installed.txt
[ "$(sudo -n sha256sum "$K" | cut -c1-64)" = "$(sha256sum Image | cut -c1-64)" ]
cmp custom.cfg "$C"
stock_hashes >post-stock.txt
cmp pre-stock.txt post-stock.txt
echo "STAGE OK $(date -u +%FT%TZ): $REL staged, stock boot files unchanged, grubenv without a one-shot"
