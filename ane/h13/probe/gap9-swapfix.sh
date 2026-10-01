#!/bin/bash
# Gap9 swap-fix: build the variant DTB dir under the per-kernel subdir the
# omarchy-ane update-m1n1 hook requires. The staged swap() flattened the
# copy (cp -a SRC DST with DST absent), which silently dropped the hook's
# state dir and made update-m1n1 embed all-stock DTBs (boot 2026-10-01
# 06:07Z had no ane node at all). Correct shape: cp -a SRC/. DST/.
set -euo pipefail
K=7.1.13-3-2-ARCH
DSTDIR=/var/lib/omarchy-ane/dtbs-nodart/$K

swapfixed() {
	sudo mkdir -p /var/lib/omarchy-ane/dtbs/$K
	sudo cp -a "$DSTDIR"/. /var/lib/omarchy-ane/dtbs/$K/
	echo "swapped (fixed); now run: sudo update-m1n1 && reboot"
}

case "${1:-}" in
	swapfixed) swapfixed ;;
	*) echo "usage: $0 swapfixed" >&2; exit 2 ;;
esac
