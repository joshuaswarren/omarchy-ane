#!/bin/bash
# Regenerate pr155-jw16-oot.tar.gz from the PR155 tree. The tarball is NOT
# committed: it is always rebuilt from the linux checkout so its contents
# are traceable to one commit.
#
# Usage: mk-tarball.sh <linux-checkout> [outdir] [commit]
#   <linux-checkout>  the omarchy-linux checkout holding the PR branch
#   [outdir]          where pr155-jw16-oot.tar.gz lands (default: here)
#   [commit]          default f088ca5c86ed96288d36753d6f87ceff84879037
#                      (PR #155 tip: "accel/ane: Declare the M2 ANE firmware files")
set -euo pipefail

LINUX=${1:?usage: mk-tarball.sh <linux-checkout> [outdir] [commit]}
COMMIT=${3:-f088ca5c86ed96288d36753d6f87ceff84879037}
KIT=$(cd "$(dirname "$0")" && pwd)
OUTDIR=$(cd "${2:-$KIT}" && pwd)

git -C "$LINUX" cat-file -e "$COMMIT^{commit}" \
	|| { echo "mk-tarball.sh: $COMMIT not in $LINUX" >&2; exit 1; }

work=$(mktemp -d /tmp/pr155-kit.XXXXXX)
trap 'rm -rf "$work"' EXIT

# Driver sources at the PR commit. The in-tree Makefile and Kconfig are
# dropped: kbuild would read them dead weight; the kit Kbuild replaces them.
git -C "$LINUX" archive "$COMMIT" -- drivers/accel/ane include/uapi/drm/ane_accel.h \
	| tar -x -C "$work"

root=$work/pr155-jw16-oot
mkdir -p "$root/ane/uapi/drm"
mv "$work"/drivers/accel/ane/* "$root/ane/"
mv "$work/include/uapi/drm/ane_accel.h" "$root/ane/uapi/drm/"
rm -f "$root/ane/Makefile" "$root/ane/Kconfig"

# Kit files from this receipts directory.
cp "$KIT/Kbuild" "$root/ane/Kbuild"
for f in build.sh gates.sh ane_get_caps.c README.md; do
	cp "$KIT/$f" "$root/$f"
done
chmod +x "$root/build.sh" "$root/gates.sh"

tar -C "$work" -czf "$OUTDIR/pr155-jw16-oot.tar.gz" pr155-jw16-oot
echo "commit:   $COMMIT ($(git -C "$LINUX" log -1 --format=%s "$COMMIT"))"
ls -l "$OUTDIR/pr155-jw16-oot.tar.gz"
sha256sum "$OUTDIR/pr155-jw16-oot.tar.gz"
