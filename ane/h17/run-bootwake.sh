#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# J700 H17 boot-wake reproduction plus the post-wake diagnostic rungs.
# Fresh boot only, gated by /var/lib/omarchy-ane/h17-bootwake.pending.
# Rung scope: power on, load firmware, release CPU, poll SCRATCH7 for
# 0x08042006 with hello_wait_ms=1000, read the IRQ status word (0x188c000)
# exactly once with irq_read=1, snapshot SCRATCH0..7 (0x1880020..0x188003c)
# once each with scratch_read=1, then SKIP the H16 RTKit management poll.
# No scratch write beyond the boot recipe, no IRQ ack write, no doorbell,
# no command path is touched. On wake the module pins itself and holds
# power: never rmmod; the host must reboot before any further rung.
set -euo pipefail

repo=${H17_REPO:-/var/lib/omarchy-ane}
ko=$repo/ane/h16/ane_h16.ko
fw=/usr/lib/firmware/apple/ane/h17_ane_fw_theia_d9x.macho
boot_id=$(cat /proc/sys/kernel/random/boot_id)
log_dir=${H17_LOG_DIR:-/var/tmp/opencode/h17-bootwake-$boot_id}
mkdir -p "$log_dir"
exec > >(tee "$log_dir/run.log") 2>&1

echo "boot_id=$boot_id kernel=$(uname -r) date=$(date -Is)"
# The service runs as root; tolerate the repo being owned by a user.
git_repo() { git -c safe.directory="$repo" -C "$repo" "$@"; }
echo "repo_head=$(git_repo rev-parse HEAD) dirty=$(git_repo status --porcelain | wc -l)"
sha256sum "$ko" "$fw"

if lsmod | grep -q '^ane_h16 '; then
    echo "FAIL: ane_h16 already loaded on this boot; rung needs a fresh boot"
    echo FAIL > "$log_dir/verdict.txt"
    exit 1
fi

before=$(dmesg --color=never | wc -l)
insmod "$ko" optin=t8140 stage=boot confirm_read=1 \
	 hello_wait_ms=1000 irq_read=1 scratch_read=1
# The wake poll completes within ~2 s (SCRATCH7 checked at 50 ms,
# hello_wait_ms=1000 caps a second pass); give the RESULT line time to land.
sleep 10
dmesg --color=never | tail -n +"$((before + 1))" > "$log_dir/dmesg-rung.log"
grep 'ane_h16' "$log_dir/dmesg-rung.log" || true
dmesg --color=never > "$log_dir/dmesg-full.log"

verdict=FAIL
reason=unknown
irq_status=missing
scratch_words=missing
# Pin the exact offsets (a regression that read the ack word 0x1890000
# or any other word must not record PASS) and require exactly one
# snapshot line of each kind (a poll-loop regression must not record
# PASS either).
irq_count=$(grep -c 'H17 IRQ_STATUS off=0x188c000 value=0x' "$log_dir/dmesg-rung.log" || true)
irq_line=$(grep -o 'H17 IRQ_STATUS off=0x188c000 value=0x[0-9a-f]*' "$log_dir/dmesg-rung.log" | tail -1 || true)
if [ -n "$irq_line" ]; then
    irq_status=$(printf '%s' "$irq_line" | sed 's/.*value=//')
fi
scratch_count=$(grep -cE 'H17 SCRATCH off=0x1880020 values=0x[0-9a-f]{8}(,0x[0-9a-f]{8}){7}' "$log_dir/dmesg-rung.log" || true)
scratch_line=$(grep -oE 'H17 SCRATCH off=0x1880020 values=0x[0-9a-f]{8}(,0x[0-9a-f]{8}){7}' "$log_dir/dmesg-rung.log" | tail -1 || true)
if [ -n "$scratch_line" ]; then
    scratch_words=$(printf '%s' "$scratch_line" | sed 's/.*values=//')
fi
if grep -q 'verdict=PASS reason=boot-wake' "$log_dir/dmesg-rung.log"; then
    if grep -q 'post-wake RTKit management poll skipped' "$log_dir/dmesg-rung.log"; then
        if [ "$irq_count" = 1 ] && [ "$scratch_count" = 1 ]; then
            verdict=PASS
            reason=wake-irq-scratch-status-and-poll-skipped
        elif [ "${irq_count:-0}" -gt 1 ] 2>/dev/null; then
            verdict=IRQ-MULTIPLE
            reason=irq-status-read-more-than-once
        elif [ "${scratch_count:-0}" -gt 1 ] 2>/dev/null; then
            verdict=SCRATCH-MULTIPLE
            reason=scratch-snapshot-logged-more-than-once
        elif [ "$irq_count" != 1 ]; then
            verdict=IRQ-MISSING
            reason=wake-without-irq-status
        else
            verdict=SCRATCH-MISSING
            reason=wake-without-scratch-snapshot
        fi
    else
        verdict=WAKE-NO-SKIP-MARKER
        reason=wake-without-skip-marker
    fi
elif grep -q 'verdict=FAIL' "$log_dir/dmesg-rung.log"; then
    reason=$(grep -o 'reason=[a-z0-9-]*' "$log_dir/dmesg-rung.log" | tail -1 || true)
fi
echo "rung=$verdict reason=$reason irq_status=$irq_status irq_lines=$irq_count scratch_words=$scratch_words scratch_lines=$scratch_count boot_id=$boot_id" | tee "$log_dir/verdict.txt"
