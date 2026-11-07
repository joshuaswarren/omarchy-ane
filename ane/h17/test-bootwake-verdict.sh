#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Verdict-grammar fixtures for run-bootwake.sh. Each fixture drives the
# REAL script (stubbed insmod/dmesg/lsmod/sha256sum/sleep/git) and the
# verdict line must classify exactly as named. A runner change that
# loosens the marker pinning fails this test.
#   run: bash test-bootwake-verdict.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

stub=$work/bin
mkdir -p "$stub"
printf '#!/bin/sh\nfor want in confirm_read=1 irq_read=1 scratch_read=1; do\n  case " $* " in *" $want "*) ;; *) echo "missing $want: $*" >&2; exit 2 ;; esac\ndone\nexit 0\n' > "$stub/insmod"
printf '#!/bin/sh\nexit 0\n' | tee "$stub/sleep" "$stub/sha256sum" "$stub/git" > /dev/null
chmod +x "$stub"/{insmod,sleep,sha256sum,git}
: > "$stub/lsmod"
chmod +x "$stub/lsmod"
printf '#!/bin/sh\nn=$(cat "$STUB_N" 2>/dev/null || echo 0); n=$((n+1)); echo "$n" > "$STUB_N"\n[ "$n" -ge 2 ] && cat "$STUB_FIXTURE"\nexit 0\n' > "$stub/dmesg"
chmod +x "$stub/dmesg"

mkdir -p "$work/repo/ane/h16"
: > "$work/repo/ane/h16/ane_h16.ko"

result='ane_h16 RESULT stage=3 soc=t8140 verdict=PASS reason=boot-wake'
skip='ane_h16 H17 post-wake RTKit management poll skipped: H16 mailbox offset is unqualified; RESULT covers wake only'
wake='ane_h16 SCRATCH7 wake word 0x8042006: firmware is running (CPU_STATUS 0x6c)'
irq1='ane_h16 H17 IRQ_STATUS off=0x188c000 value=0x00000000 (single read-only post-wake snapshot; ack register untouched)'
scratch1='ane_h16 H17 SCRATCH off=0x1880020 values=0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000001,0x08042006 (single read-only post-wake snapshot; scratch words untouched)'
scratch2='ane_h16 H17 SCRATCH off=0x1880020 values=0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000 (single read-only post-wake snapshot; scratch words untouched)'
scratch3='ane_h16 H17 SCRATCH off=0x1880020 values=0xdeadbeef,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000001,0x08042006 (single read-only post-wake snapshot; scratch words untouched)'

fails=0
check() { # name fixture verdict-substr
	local name=$1 fixture=$2 expect=$3
	local logdir=$work/log-$name
	mkdir -p "$logdir"
	printf '%s\n' "$fixture" > "$work/fix-$name.log"
	echo 0 > "$work/counter-$name"
	if ! STUB_N=$work/counter-$name STUB_FIXTURE=$work/fix-$name.log \
	   PATH=$stub:$PATH H17_REPO=$work/repo H17_LOG_DIR=$logdir \
	   bash "$here/run-bootwake.sh" > "$work/run-$name.log" 2>&1; then
		printf '%-18s FAIL runner exited nonzero: %s\n' "$name" "$(tail -1 "$work/run-$name.log")"
		fails=$((fails + 1))
		return
	fi
	local got
	got=$(cat "$logdir/verdict.txt")
	if printf '%s' "$got" | grep -q "$expect"; then
		printf '%-18s PASS %s\n' "$name" "$got"
	else
		printf '%-18s FAIL expected %s got %s\n' "$name" "$expect" "$got"
		fails=$((fails + 1))
	fi
}

# 1: every marker exactly once, RESULT PASS -> PASS
check pass "$wake
$irq1
$scratch3
$skip
$result" 'rung=PASS reason=wake-irq-scratch-status-and-poll-skipped irq_status=0x00000000 irq_lines=1 scratch_words=0xdeadbeef,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000001,0x08042006 scratch_lines=1'

# 2: two scratch snapshot lines -> SCRATCH-MULTIPLE
check scratch-multi "$wake
$irq1
$scratch1
$scratch2
$skip
$result" 'rung=SCRATCH-MULTIPLE reason=scratch-snapshot-logged-more-than-once'

# 3: no scratch snapshot -> SCRATCH-MISSING
check scratch-missing "$wake
$irq1
$skip
$result" 'rung=SCRATCH-MISSING reason=wake-without-scratch-snapshot'

# 4: no IRQ line -> IRQ-MISSING (checked before SCRATCH-MISSING)
check irq-missing "$wake
$scratch3
$skip
$result" 'rung=IRQ-MISSING reason=wake-without-irq-status'

# 5: markers at wrong offsets (ack word, past the scratch block) ->
#    counts stay 0 -> IRQ-MISSING; a wrong offset must never PASS.
check wrong-offset "$wake
ane_h16 H17 IRQ_STATUS off=0x1890000 value=0x00000001 (bogus)
ane_h16 H17 SCRATCH off=0x1880040 values=0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000,0x00000000 (bogus)
$skip
$result" 'rung=IRQ-MISSING'

# 6: RESULT PASS without the skip marker -> WAKE-NO-SKIP-MARKER
check no-skip "$wake
$irq1
$scratch3
$result" 'rung=WAKE-NO-SKIP-MARKER reason=wake-without-skip-marker'

echo "== test-bootwake-verdict: $fails failure$( [ "$fails" = 1 ] || echo s) =="
[ "$fails" = 0 ]
