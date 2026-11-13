#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren
#
# test_private_tmpdir.sh: test_libane_ioctl must build its synthetic
# lookup programs in a per-run private mkdtemp directory (honouring
# $TMPDIR), never at fixed shared /tmp names. Two concurrent
# `make -C tools check` runs on one host used to share
# /tmp/t6021-lookup-p*.anec; one run loaded the other's file mid-write
# and segfaulted. The occupant check proves the fixed names are not
# used; the concurrent check proves two instances coexist and that
# nothing is left behind. usage: sh test_private_tmpdir.sh
# [binary] [fixtures-dir]   (run from tools/)
set -eu
BIN=${1:-./test_libane_ioctl}
FIX=${2:-../fixtures/h14-anec}
fail=0
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# An ordinary concurrent run -- or a crashed one -- leaves its file at
# the fixed shared name. A directory there cannot be opened for
# writing, so any use of the fixed name fails the run.
for name in /tmp/t6021-lookup-p00.anec /tmp/t6021-lookup-alias.anec; do
	mkdir "$name"
done
if "$BIN" "$FIX" >"$tmp/occupant.log" 2>&1; then
	echo "ok: run with an occupant at the fixed /tmp names passed"
else
	echo "FAIL private tmpdir: the test still writes the fixed shared" \
	     "/tmp/t6021-lookup-* names (occupant broke the run):"
	tail -n 5 "$tmp/occupant.log"
	fail=1
fi
rmdir /tmp/t6021-lookup-p00.anec /tmp/t6021-lookup-alias.anec

# Two concurrent instances: both must pass and leave nothing behind.
TMPDIR="$tmp" "$BIN" "$FIX" >"$tmp/c1.log" 2>&1 & p1=$!
"$BIN" "$FIX" >"$tmp/c2.log" 2>&1 & p2=$!
r1=0
r2=0
wait "$p1" || r1=$?
wait "$p2" || r2=$?
if [ "$r1" -eq 0 ] && grep -q 'IOCTL-CHECK PASS' "$tmp/c1.log" &&
   [ "$r2" -eq 0 ] && grep -q 'IOCTL-CHECK PASS' "$tmp/c2.log"; then
	echo "ok: two concurrent instances both passed"
else
	echo "FAIL private tmpdir: concurrent instances interfere" \
	     "(exit $r1/$r2):"
	tail -n 5 "$tmp/c1.log" "$tmp/c2.log"
	fail=1
fi
if [ -z "$(ls /tmp/t6021-lookup-* "$tmp"/t6021-lookup-* 2>/dev/null)" ]; then
	echo "ok: no lookup files left in /tmp or TMPDIR"
else
	echo "FAIL private tmpdir: lookup files left behind:" \
	     "$(ls /tmp/t6021-lookup-* "$tmp"/t6021-lookup-* 2>/dev/null)"
	fail=1
fi

exit "$fail"
