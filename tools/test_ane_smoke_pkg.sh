#!/usr/bin/env bash
# shellcheck disable=SC2016,SC2034
# test_ane_smoke_pkg.sh: the omarchy-ane-smoke pacman package builds and installs the smoke
# program, the omarchy-ane-run runner, and both add fixtures, so the qualification kit's bare
# `omarchy-ane-smoke` call resolves on an installed image instead of exiting 127.
# Run from anywhere: bash tools/test_ane_smoke_pkg.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
PKGDIR="$REPO/packaging/arch/omarchy-ane-smoke"
fails=0
chk() { if eval "$2"; then echo "PASS: $1"; else echo "FAIL: $1"; fails=$((fails+1)); fi; }

[ -d "$PKGDIR" ] || { echo "FAIL: $PKGDIR missing"; exit 1; }
chk "PKGBUILD parses under bash -n" 'bash -n "$PKGDIR/PKGBUILD"'

OUT=$(mktemp -d /tmp/ane-smoke-pkg.XXXXXX)
cleanup() { find "$OUT" -mindepth 1 -delete; rmdir "$OUT" 2>/dev/null || true; }
trap cleanup EXIT
cd "$PKGDIR" || exit 1
# --nodeps: this container has no pacman database; on a real Arch system plain makepkg works.
if ! makepkg -f --nodeps >"$OUT/makepkg.log" 2>&1; then
  echo "FAIL: makepkg"; tail -n 4 "$OUT/makepkg.log"; exit 1
fi
pkg=$(ls ./*.pkg.tar.* 2>/dev/null | head -n 1)
[ -n "$pkg" ] || { echo "FAIL: makepkg produced no package"; exit 1; }
chk "the package carries usr/bin/omarchy-ane-smoke" 'tar -tf "$pkg" | grep -q "usr/bin/omarchy-ane-smoke$"'
chk "the package carries usr/bin/omarchy-ane-run" 'tar -tf "$pkg" | grep -q "usr/bin/omarchy-ane-run$"'
chk "the package carries the h13 add fixture" 'tar -tf "$pkg" | grep -q "usr/share/omarchy-ane/fixtures/h13-anec/add/program-0.anec$"'
chk "the package carries the h14 add fixture" 'tar -tf "$pkg" | grep -q "usr/share/omarchy-ane/fixtures/h14-anec/add/program-0.anec$"'
chk "the packaged script is the repo copy (diff)" 'tar -xOf "$pkg" usr/bin/omarchy-ane-smoke | diff -q - "$REPO/packaging/omarchy-ane-smoke" >/dev/null'
chk "the packaged runner runs (--help prints usage, exits 2)" 'tar -xOf "$pkg" usr/bin/omarchy-ane-run > "$OUT/ane-run" && chmod +x "$OUT/ane-run" && help_out=$("$OUT/ane-run" --help 2>&1); [ $? = 2 ] && printf "%s" "$help_out" | grep -q usage'

# Clean-install entry check: the installed program, against an empty fake root, RESOLVES as a
# command and reports unavailable (exit 2) with valid JSON — the kit's 127 is gone. A bound
# driver plus these packaged pieces is then all a real machine needs for exit 0.
TMPROOT=$(mktemp -d /tmp/ane-smoke-root.XXXXXX)
mkdir -p "$TMPROOT/usr/bin"
tar -xOf "$pkg" usr/bin/omarchy-ane-smoke > "$TMPROOT/usr/bin/omarchy-ane-smoke"
chmod +x "$TMPROOT/usr/bin/omarchy-ane-smoke"
out=$(cd "$TMPROOT" && "$TMPROOT/usr/bin/omarchy-ane-smoke" --root "$TMPROOT" 2>/dev/null); rc=$?
chk "the packaged smoke resolves on a fake root and exits 2 (was 127 before the package)" '[ "$rc" = 2 ]'
chk "the unavailable line is valid JSON with available:false" 'python3 -c "import json,sys; d=json.loads(sys.argv[1]); assert d[\"available\"] is False and d[\"chip\"] is None" "$out"'
chk "the fake root stayed untouched (read-only)" '[ "$(find "$TMPROOT" -newer "$TMPROOT/usr/bin/omarchy-ane-smoke" -type f | grep -vc omarchy-ane-smoke || true)" = 0 ]'

echo "selftest: $fails failure(s)"; exit $((fails > 0))
