#!/bin/bash
# shellcheck disable=SC2024 # sudo output goes to the caller's own files in STAGEDIR
# Put the in-tree board device tree into m1n1's boot.bin. This is the only path for the in-tree DT nodes:
# m1n1 stage 2 picks the board tree out of the ESP boot.bin, patches it from the ADT and hands it to
# U-Boot; GRUB has no devicetree line, so a GRUB entry cannot change the tree. Only the board tree changes:
# the m1n1 part, the other trees and U-Boot stay byte-identical. The new tree is the staged in-tree DTB
# plus this host's opted-in omarchy-ane overlays (omarchy-ane-dt apply in a scratch root, as OwnMemGate's
# DTB prediction); its skip-if-compatible rule leaves the ANE overlay out, because the tree has the node.
# The ESP change applies to EVERY GRUB entry, the stock one included, until restore.
# usage: bootbin.sh make STAGEDIR OMARCHY_ANE_TREE   STAGEDIR/boot.bin.intree from the current boot.bin
#        bootbin.sh install STAGEDIR                 ESP backup, write, readback, sync, 45 s, sync, readback
#        bootbin.sh restore STAGEDIR                 write the boot.bin that make started from (same discipline)
# make reads BOOTBIN (default the ESP file), COMPAT (default the running board compatible) and OPTIN
# (default /etc/omarchy-platform/dtb-overlays.opt-in), so it also runs off the host on copies.
set -euo pipefail
CMD=${1:?make|install|restore}
S=$(realpath "${2:?stagedir}")
E=/boot/efi/m1n1/boot.bin
NEW=$S/boot.bin.intree
OLD=$S/boot.bin.base

put() { # FILE SHA
	sudo -n cp "$1" "$E"
	sync
	echo "$2  $E" | sudo -n sha256sum -c -
	sleep 45
	sync
	echo "$2  $E" | sudo -n sha256sum -c -
}

case $CMD in
make)
	T=$(realpath "${3:?omarchy-ane tree}")
	B=${BOOTBIN:-$E}
	C=${COMPAT:-/sys/firmware/devicetree/base/compatible}
	P=${OPTIN:-/etc/omarchy-platform/dtb-overlays.opt-in}
	D=$S/dtroot
	REL=$(cat "$S/release")
	if [ -e "$D" ] || [ -e "$NEW" ]; then echo "exists: $D or $NEW"; exit 2; fi
	if [ -r "$B" ]; then cp "$B" "$OLD"; else sudo -n cat "$B" >"$OLD"; fi
	board=$(tr '\0' '\n' <"$C" | grep -m1 -E '^apple,j[0-9a-z]+$' | cut -d, -f2)
	name=$(tr '\0' '\n' <"$C" | grep -m1 -E '^apple,t[0-9]+$' | cut -d, -f2)-$board.dtb
	mkdir -p "$D/sys/firmware/devicetree/base" "$D/usr/lib/modules/$REL/dtbs" "$D/etc/omarchy-platform"
	cp "$C" "$D/sys/firmware/devicetree/base/compatible"
	cp "$S/dtbs/$name" "$D/usr/lib/modules/$REL/dtbs/"
	if [ -f "$P" ]; then cp "$P" "$D/etc/omarchy-platform/dtb-overlays.opt-in"; fi
	"$T/packaging/build-dtbo" "$D"
	python3 "$T/packaging/omarchy-ane-dt" apply --root "$D"
	tree=$D/var/lib/omarchy-ane/dtbs/$REL/$name
	[ -f "$tree" ] || tree=$S/dtbs/$name # no overlay applies on this host
	python3 - "$OLD" "$tree" "$NEW" "$board" "$S/tree-base.dtb" <<'PY'
import hashlib, struct, sys, zlib
old, tree, out, board, base_tree = sys.argv[1:]
data, new = open(old, "rb").read(), open(tree, "rb").read()
sha = lambda b: hashlib.sha256(b).hexdigest()
def parts(d):  # bootbin_parse.py (DiskBoot): m1n1 is larger than 1 MiB, then contiguous trees, then gzip U-Boot
    dtbs, pos = [], 0x100000
    while (pos := d.find(b"\xd0\x0d\xfe\xed", pos)) != -1:
        size, ver = struct.unpack_from(">I", d, pos + 4)[0], struct.unpack_from(">I", d, pos + 20)[0]
        if 0x100 < size < 0x100000 and ver == 17:
            dtbs.append((pos, size))
            pos += size
        else:
            pos += 4
    assert all(a + n == b for (a, n), (b, _) in zip(dtbs, dtbs[1:])), "trees not contiguous"
    gz = zlib.decompressobj(16 + zlib.MAX_WBITS)
    uboot = gz.decompress(d[dtbs[-1][0] + dtbs[-1][1]:])
    return dtbs, uboot, gz.unused_data
key = f"apple,{board}\0".encode()
dtbs, uboot, tail = parts(data)
hits = [(o, n) for o, n in dtbs if key in data[o:o + n]]
assert len(hits) == 1, f"{len(hits)} {board} trees"
assert key in new and struct.unpack_from(">I", new, 4)[0] == len(new) < 0x100000, "bad new tree"
o, n = hits[0]
res = data[:o] + new + data[o + n:]
rdtbs, ruboot, rtail = parts(res)
assert res[:dtbs[0][0]] == data[:dtbs[0][0]] and (ruboot, rtail) == (uboot, tail) and len(rdtbs) == len(dtbs)
open(out, "wb").write(res)
open(base_tree, "wb").write(data[o:o + n])
print(f"base   {sha(data)} {len(data)} B: m1n1 {dtbs[0][0]} B {sha(data[:dtbs[0][0]])}, {len(dtbs)} trees, u-boot {sha(uboot)}")
print(f"{board} tree {sha(data[o:o + n])} {n} B -> {sha(new)} {len(new)} B at {o:#x}")
print(f"intree {sha(res)} {len(res)} B: m1n1, the other {len(dtbs) - 1} trees and u-boot byte-identical")
PY
	dtc -q -I dtb -O dts -s -o "$S/tree-base.dts" "$S/tree-base.dtb"
	dtc -q -I dtb -O dts -s -o "$S/tree-intree.dts" "$tree"
	diff -u "$S/tree-base.dts" "$S/tree-intree.dts" >"$S/tree.diff" || true
	echo "tree diff: $(grep -c '^[-+][^-+]' "$S/tree.diff") changed lines (tree.diff); new tree: ane $(fdtget -t s "$tree" /soc/ane@284000000 compatible 2>/dev/null || echo -), /config bootcmd $(fdtget -t s "$tree" /config bootcmd 2>/dev/null || echo -)"
	(cd "$S" && sha256sum boot.bin.base boot.bin.intree) | tee "$S/boot.bin.sha256"
	echo BOOTBIN-MAKE-OK
	;;
install)
	old=$(sha256sum "$OLD" | cut -c1-64)
	new=$(sha256sum "$NEW" | cut -c1-64)
	echo "$old  $E" | sudo -n sha256sum -c - # the ESP still holds the boot.bin that make started from
	bak=$E.${old:0:8}.bak
	sudo -n test -e "$bak" || sudo -n cp -p "$E" "$bak"
	echo "$old  $bak" | sudo -n sha256sum -c -
	put "$NEW" "$new"
	df -h /boot/efi | tail -1
	echo "BOOTBIN-INSTALL-OK $(date -u +%FT%TZ): ESP $new, backup $bak"
	;;
restore)
	old=$(sha256sum "$OLD" | cut -c1-64)
	put "$OLD" "$old"
	echo "BOOTBIN-RESTORE-OK $(date -u +%FT%TZ): ESP $old"
	;;
*) echo "bad command $CMD"; exit 2 ;;
esac
