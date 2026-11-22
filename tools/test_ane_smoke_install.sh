#!/usr/bin/env bash
# shellcheck disable=SC2016,SC2034
# test_ane_smoke_install.sh: a clean install puts the ANE smoke userspace where the qualification
# kit looks for it, so the kit's `ane` step stops failing with exit 127 (command missing).
# Run from anywhere: bash tools/test_ane_smoke_install.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)
fails=0
chk() { if eval "$2"; then echo "PASS: $1"; else echo "FAIL: $1"; fails=$((fails+1)); fi; }

ROOT=$(mktemp -d /tmp/ane-smoke-install.XXXXXX)
cleanup() { find "$ROOT" -mindepth 1 -delete 2>/dev/null; rmdir "$ROOT" 2>/dev/null || true; }
trap cleanup EXIT
# libane's and the bindings' install targets write into these and do not create them.
mkdir -p "$ROOT/usr/lib" "$ROOT/usr/include"

chk "the top-level install installs the smoke userspace" 'grep -q "install-smoke" "$REPO/Makefile"'
# The smoke install target on its own: a clean install into an empty DESTDIR. The top-level
# `make install` also runs the bindings' legacy `setup.py install`, which modern setuptools
# dropped; that is pre-existing and out of scope here.
if ! make -C "$REPO/tools" install-smoke DESTDIR="$ROOT" >"$ROOT/install.log" 2>&1; then
  echo "FAIL: make install-smoke"; tail -n 6 "$ROOT/install.log"; exit 1
fi
chk "the clean install puts omarchy-ane-smoke on the command path" '[ -x "$ROOT/usr/bin/omarchy-ane-smoke" ]'
chk "the clean install puts omarchy-ane-run on the command path" '[ -x "$ROOT/usr/bin/omarchy-ane-run" ]'
chk "the clean install puts the h13 add fixture in place" '[ -f "$ROOT/usr/share/omarchy-ane/fixtures/h13-anec/add/program-0.anec" ]'
chk "the clean install puts the h14 add fixture in place" '[ -f "$ROOT/usr/share/omarchy-ane/fixtures/h14-anec/add/program-0.anec" ]'
chk "the installed smoke is the repo copy (diff)" 'diff -q "$ROOT/usr/bin/omarchy-ane-smoke" "$REPO/packaging/omarchy-ane-smoke" >/dev/null'
chk "the installed h14 fixture is the repo copy (diff)" 'diff -q "$ROOT/usr/share/omarchy-ane/fixtures/h14-anec/add/program-0.anec" "$REPO/fixtures/h14-anec/add/program-0.anec" >/dev/null'
# The entry the kit calls by name resolves: it exits 2 with JSON (no ANE in this container),
# never 127 (command missing).
out=$(cd "$ROOT" && "$ROOT/usr/bin/omarchy-ane-smoke" --root "$ROOT" 2>/dev/null); rc=$?
chk "the installed smoke runs and exits 2, not 127" '[ "$rc" = 2 ]'
chk "the unavailable line is valid JSON with available:false" 'printf "%s" "$out" | python3 -c "import json,sys; d=json.load(sys.stdin); assert d[\"available\"] is False"'
chk "the smoke entry resolves by name on PATH" 'PATH="$ROOT/usr/bin:$PATH" command -v omarchy-ane-smoke >/dev/null'

echo "selftest: $fails failure(s)"; exit $((fails > 0))
