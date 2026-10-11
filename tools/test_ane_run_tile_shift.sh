#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren
#
# Offline checks for ane-run --tile-shift (no device is opened):
#   1. a shift outside 1..20 is refused with exit 2 before any load;
#   2. --tile-shift together with --ports is refused with exit 2 (index mode only);
#   3. the usage text names the option;
#   4. a valid shift reaches the load: with no such file the exit is 1 and the message is ane_init failed, not a usage error.
# This test guards option parsing and the refusals. It does not show that the shift reaches libane:
# with a missing file both init calls fail the same way. The shift itself is shown by the device runs in
# receipts/2026-10-11-m1-parakeet-tile-shift/.
# Usage: test_ane_run_tile_shift.sh ./ane-run
set -u
run=${1:-./ane-run}
bad=0
check() { # check NAME EXPECTED_RC ACTUAL_RC
	if [ "$2" = "$3" ]; then echo "PASS $1"; else echo "FAIL $1: exit $3, wanted $2"; bad=1; fi
}
out=$("$run" --anec /nonexistent --tile-shift 0 2>&1); check "shift 0 refused" 2 $?
case $out in *"--tile-shift must be 1..20"*) echo "PASS shift 0 message" ;; *) echo "FAIL shift 0 message: $out"; bad=1 ;; esac
out=$("$run" --anec /nonexistent --tile-shift 21 2>&1); check "shift 21 refused" 2 $?
printf '{"schema_version": 2, "program": "p", "anec": "x", "ports": []}\n' > /tmp/ane-run-ts-ports.$$.json
out=$("$run" --anec /nonexistent --ports /tmp/ane-run-ts-ports.$$.json --tile-shift 9 2>&1); rc=$?
rm -f /tmp/ane-run-ts-ports.$$.json
check "ports plus tile-shift refused" 2 "$rc"
out=$("$run" --bogus 2>&1); check "usage exit" 2 $?
case $out in *"--tile-shift"*) echo "PASS usage names --tile-shift" ;; *) echo "FAIL usage lacks --tile-shift"; bad=1 ;; esac
out=$("$run" --anec /nonexistent/program.anec --tile-shift 9 2>&1); check "valid shift reaches the load" 1 $?
case $out in *"ane_init failed"*|*"LIBANE"*|*"cannot"*|*"No such"*) echo "PASS load error, not a usage error" ;; *) echo "FAIL unexpected output: $out"; bad=1 ;; esac
exit $bad
