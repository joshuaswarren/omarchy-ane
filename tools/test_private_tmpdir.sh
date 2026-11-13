#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren
#
# test_private_tmpdir.sh: test_libane_ioctl must build its synthetic
# lookup programs in a per-run private mkdtemp directory (honouring
# $TMPDIR), never at fixed shared /tmp names. Two concurrent
# `make -C tools check` runs on one host used to share
# /tmp/t6021-lookup-p*.anec; one run loaded the other's file mid-write
# and segfaulted. The binary reports its lookup directory through
# T6021_LOOKUP_DIR_REPORT; this script runs two instances in separate
# private TMPDIRs and asserts each reported, used and cleaned its own.
# Nothing outside this script's own scratch tree is created or
# checked, so concurrent copies of the script cannot interfere.
# usage: sh test_private_tmpdir.sh [binary] [fixtures-dir]   (run from tools/)
set -eu
BIN=${1:-./test_libane_ioctl}
FIX=${2:-../fixtures/h14-anec}
fail=0

# One scratch tree for everything this invocation creates; the trap is
# set before any file or directory is made and removes only it.
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
mkdir "$scratch/p1" "$scratch/p2"

TMPDIR="$scratch/p1" T6021_LOOKUP_DIR_REPORT="$scratch/report1" \
	"$BIN" "$FIX" >"$scratch/run1.log" 2>&1 & q1=$!
TMPDIR="$scratch/p2" T6021_LOOKUP_DIR_REPORT="$scratch/report2" \
	"$BIN" "$FIX" >"$scratch/run2.log" 2>&1 & q2=$!
r1=0
r2=0
wait "$q1" || r1=$?
wait "$q2" || r2=$?

for i in 1 2; do
	log="$scratch/run$i.log"
	rep="$scratch/report$i"
	dir="$scratch/p$i"
	ri=$(eval "echo \${r$i}")
	if [ "$ri" -eq 0 ] && grep -q 'IOCTL-CHECK PASS' "$log" &&
	   grep -q "^$dir/t6021-lookup-" "$rep" 2>/dev/null; then
		echo "ok: instance $i built its lookup programs in its private TMPDIR"
	else
		echo "FAIL private tmpdir: instance $i did not use its" \
		     "private TMPDIR (exit $ri):"
		tail -n 5 "$log"
		fail=1
	fi
	if [ -z "$(find "$dir" -mindepth 1 -print -quit 2>/dev/null)" ]; then
		echo "ok: instance $i left its private TMPDIR empty"
	else
		echo "FAIL private tmpdir: instance $i left files in its" \
		     "private TMPDIR:"
		find "$dir" -mindepth 1
		fail=1
	fi
done

exit "$fail"
