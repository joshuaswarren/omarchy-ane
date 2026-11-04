#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# J700 H17 boot-wake reproduction with the poll-skip build (commit 91eb462).
# Fresh boot only, gated by /var/lib/omarchy-ane/h17-bootwake.pending.
# Rung scope: power on, load firmware, release CPU, poll SCRATCH7 for
# 0x08042006 with hello_wait_ms=1000, then SKIP the H16 RTKit management
# poll. No command path is touched. On wake the module pins itself and
# holds power: never rmmod; the host must reboot before any further rung.
set -euo pipefail

repo=${H17_REPO:-/home/admin/omarchy-ane}
ko=$repo/ane/h16/ane_h16.ko
fw=/usr/lib/firmware/apple/ane/h17_ane_fw_theia_d9x.macho
boot_id=$(cat /proc/sys/kernel/random/boot_id)
log_dir=${H17_LOG_DIR:-/var/tmp/opencode/h17-bootwake-$boot_id}
mkdir -p "$log_dir"
exec > >(tee "$log_dir/run.log") 2>&1

echo "boot_id=$boot_id kernel=$(uname -r) date=$(date -Is)"
echo "repo_head=$(git -C "$repo" rev-parse HEAD) dirty=$(git -C "$repo" status --porcelain | wc -l)"
sha256sum "$ko" "$fw"

if lsmod | grep -q '^ane_h16 '; then
    echo "FAIL: ane_h16 already loaded on this boot; rung needs a fresh boot"
    echo FAIL > "$log_dir/verdict.txt"
    exit 1
fi

before=$(dmesg --color=never | wc -l)
insmod "$ko" optin=t8140 stage=boot hello_wait_ms=1000
# The wake poll completes within ~2 s (SCRATCH7 checked at 50 ms,
# hello_wait_ms=1000 caps a second pass); give the RESULT line time to land.
sleep 10
dmesg --color=never | tail -n +"$((before + 1))" > "$log_dir/dmesg-rung.log"
grep 'ane_h16' "$log_dir/dmesg-rung.log" || true
dmesg --color=never > "$log_dir/dmesg-full.log"

verdict=FAIL
reason=unknown
if grep -q 'verdict=PASS reason=boot-wake' "$log_dir/dmesg-rung.log"; then
    if grep -q 'post-wake RTKit management poll skipped' "$log_dir/dmesg-rung.log"; then
        verdict=PASS
        reason=wake-and-poll-skipped
    else
        verdict=WAKE-NO-SKIP-MARKER
        reason=wake-without-skip-marker
    fi
elif grep -q 'verdict=FAIL' "$log_dir/dmesg-rung.log"; then
    reason=$(grep -o 'reason=[a-z0-9-]*' "$log_dir/dmesg-rung.log" | tail -1)
fi
echo "rung=$verdict reason=$reason boot_id=$boot_id" | tee "$log_dir/verdict.txt"
