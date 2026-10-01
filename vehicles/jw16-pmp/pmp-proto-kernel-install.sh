#!/bin/bash
# pmp-proto-kernel-install.sh — stage/install/revert the Jw16PmpProto candidate
# kernel on jw16 (second GRUB entry; standard entry stays default).
#
# Mode is the first argument: stage | install | revert | check
#   stage   : fetch the built vmlinuz from studio and pin+verify its SHA256
#   install : /boot/vmlinuz-linux-asahi-pmpproto + /etc/grub.d/41_pmpproto +
#             grub-mkconfig (append-only; stock entries untouched)
#   revert  : remove the pmpproto kernel + entry, regen grub.cfg
#   check   : read-only state dump
#
# One-shot boot = `grub-reboot '<title>'` (GRUB_DEFAULT=saved already set);
# a plain reboot always returns to the stock kernel.
set -euo pipefail
MODE=${1:?mode: stage|install|revert|check}
EXPECT_SHA=${2:-}
STAGE_DIR=/var/tmp/ane-pmpproto
KIMG=/boot/vmlinuz-linux-asahi-pmpproto
STUDIO_VLZ=/var/tmp/pmp-proto-build/arch/arm64/boot/Image
ESP_UUID=4f4d5801-424f-4f54-8000-000000000001
ROOT_UUID=4f4d5801-524f-4f54-8000-000000000001
CMDLINE='root=UUID=4f4d5801-524f-4f54-8000-000000000001 rw rootflags=subvol=@ zswap.enabled=0 rootfstype=btrfs quiet loglevel=3 splash'
TITLE='Jw16PmpProto candidate (pmp PM protocol)'

case "$MODE" in
stage)
	[ -n "$EXPECT_SHA" ] || { echo "stage needs EXPECT_SHA"; exit 1; }
	mkdir -p "$STAGE_DIR"
	rsync -a omp-studio-local:"$STUDIO_VLZ" "$STAGE_DIR/vmlinuz-pmpproto"
	GOT=$(sha256sum "$STAGE_DIR/vmlinuz-pmpproto" | awk '{print $1}')
	echo "$GOT" > "$STAGE_DIR/vmlinuz.sha"
	[ "$GOT" = "$EXPECT_SHA" ] || { echo "FATAL sha mismatch got=$GOT want=$EXPECT_SHA"; exit 1; }
	echo "STAGED OK sha=$GOT"
	;;
install)
	GOT=$(sha256sum "$STAGE_DIR/vmlinuz-pmpproto" | awk '{print $1}')
	[ "$GOT" = "$(cat "$STAGE_DIR/vmlinuz.sha")" ] || { echo "FATAL staged sha drifted"; exit 1; }
	[ "$(id -u)" = 0 ] || { echo "run install as root (sudo)"; exit 1; }
	cp "$STAGE_DIR/vmlinuz-pmpproto" "$KIMG"
	cat > /etc/grub.d/41_pmpproto <<EOF
#!/bin/sh
exec tail -n +3 \$0
menuentry '$TITLE' {
	load_video
	set gfxpayload=keep
	insmod gzio
	insmod part_gpt
	insmod ext2
	search --no-floppy --fs-uuid --set=root $ESP_UUID
	echo	'Loading Jw16PmpProto candidate kernel ...'
	linux	/vmlinuz-linux-asahi-pmpproto $CMDLINE
	initrd	/initramfs-linux-asahi.img
}
EOF
	chmod +x /etc/grub.d/41_pmpproto
	grub-mkconfig -o /boot/grub/grub.cfg
	sha256sum "$KIMG"
	echo "INSTALLED. One-shot: grub-reboot '$TITLE' && systemctl reboot"
	;;
revert)
	[ "$(id -u)" = 0 ] || { echo "run revert as root (sudo)"; exit 1; }
	rm -f /etc/grub.d/41_pmpproto "$KIMG"
	grub-mkconfig -o /boot/grub/grub.cfg
	grub-editenv /boot/grub/grubenv unset next_entry 2>/dev/null || true
	echo "REVERTED (stock entries were never modified)"
	;;
check)
	ls -la "$KIMG" 2>/dev/null && sha256sum "$KIMG" || echo "no pmpproto kernel installed"
	echo -n "staged: "; cat "$STAGE_DIR/vmlinuz.sha" 2>/dev/null || echo "nothing staged"
	grub-editenv list 2>/dev/null || true
	ls /etc/grub.d/41_pmpproto 2>/dev/null || echo "no 41_pmpproto entry"
	;;
*) echo "unknown mode $MODE"; exit 1;;
esac
