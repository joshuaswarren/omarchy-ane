#!/usr/bin/env bash
# shellcheck disable=SC2016,SC2034
# test_ane_check_pkg.sh: the omarchy-ane-check pacman package builds and installs the real check.
# Run from anywhere: bash tools/test_ane_check_pkg.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
PKGDIR="$REPO/packaging/arch/omarchy-ane-check"
fails=0
chk() { if eval "$2"; then echo "PASS: $1"; else echo "FAIL: $1"; fails=$((fails+1)); fi; }

[ -d "$PKGDIR" ] || { echo "FAIL: $PKGDIR missing"; exit 1; }
chk "PKGBUILD parses under bash -n" 'bash -n "$PKGDIR/PKGBUILD"'

OUT=$(mktemp -d /tmp/ane-check-pkg.XXXXXX)
cleanup() { find "$OUT" -mindepth 1 -delete; rmdir "$OUT" 2>/dev/null || true; }
trap cleanup EXIT
cd "$PKGDIR" || exit 1
# --nodeps: this container has no pacman database; on a real Arch system plain makepkg works.
if ! makepkg -f --nodeps >"$OUT/makepkg.log" 2>&1; then
  echo "FAIL: makepkg"; tail -n 4 "$OUT/makepkg.log"; exit 1
fi
pkg=$(ls ./*.pkg.tar.* 2>/dev/null | head -n 1)
[ -n "$pkg" ] || { echo "FAIL: makepkg produced no package"; exit 1; }
chk "the package carries usr/bin/omarchy-ane-check" 'tar -tf "$pkg" | grep -q "usr/bin/omarchy-ane-check$"'
chk "the package carries the ane-probe doc" 'tar -tf "$pkg" | grep -q "usr/share/doc/omarchy-ane-check/ane-probe.md$"'
chk "the packaged script is the repo copy (diff)" 'tar -xOf "$pkg" usr/bin/omarchy-ane-check | diff -q - "$REPO/packaging/omarchy-ane-check" >/dev/null'
chk "shellcheck clean on the check script" 'shellcheck -S warning "$REPO/packaging/omarchy-ane-check"'
# the installed script runs read-only against an empty fake root and fails honestly (no kernel inside)
TMPROOT=$(mktemp -d /tmp/ane-check-root.XXXXXX)
tar -xOf "$pkg" usr/bin/omarchy-ane-check > "$TMPROOT/omarchy-ane-check"
out=$(bash "$TMPROOT/omarchy-ane-check" --root "$TMPROOT" 2>&1); rc=$?
chk "the packaged script runs read-only on a fake root and exits 1 with FAIL lines" '[ "$rc" = 1 ] && printf "%s" "$out" | grep -q "FAIL"'
chk "the packaged script never touches the fake root (read-only)" '[ "$(find "$TMPROOT" -newer "$TMPROOT/omarchy-ane-check" -type f | grep -vc omarchy-ane-check || true)" = 0 ]'

echo "selftest: $fails failure(s)"; exit $((fails > 0))
