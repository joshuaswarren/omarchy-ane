#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# J700 H17 stage 4: bounded firmware wake. Run with no argument for the
# read-only ASC snapshot only; pass GO as the first argument to also do the
# confirm_wake=1 run (one RUN write, bounded polls, mailbox peek, RUN clear).
# Stop at the first failed rung. A hang costs one reboot.
set -euo pipefail

repo=${H17_REPO:-/home/admin/omarchy-ane}
module_dir=$repo/ane/h17
boot_id=$(cat /proc/sys/kernel/random/boot_id)
log_dir=${H17_LOG_DIR:-/var/log/omarchy-ane}
mkdir -p "$log_dir"
exec > >(tee "$log_dir/h17-stage4-$boot_id.log") 2>&1

cleanup() {
    if lsmod | grep -q '^ane_h17_wake '; then
        rmmod ane_h17_wake || true
    fi
}
trap cleanup EXIT

wake_rung() {
    shift
    local before lines
    before=$(dmesg --color=never | wc -l)
    insmod "$module_dir/ane_h17_wake.ko" optin=1 "$@"
    lines=$(dmesg --color=never | tail -n +"$((before + 1))")
    printf '%s\n' "$lines" | grep 'ane_h17_wake'
    if printf '%s\n' "$lines" | grep -Fq 'reason=core-not-stopped'; then
        echo 'H17_STAGE4_CORE_NOT_STOPPED domain stays on; a reboot is required'
        exit 3
    fi
    printf '%s\n' "$lines" | grep -Fq "RESULT stage=4 verdict=PASS"
    rmmod ane_h17_wake
}

printf 'boot_id=%s kernel=%s repo=%s\n' "$boot_id" "$(uname -r)" "$(git -C "$repo" rev-parse HEAD)"
test "$(uname -r)" = 7.2.11-6-1-ARCH
test "$(tr -d '\000' < /proc/device-tree/model)" = 'Apple MacBook Neo'
test -z "$(lsmod | grep '^ane_h17_')"
python3 "$module_dir/preflight.py"
test -e /proc/device-tree/soc/ane-h17-ps-probe/compatible || {
    echo 'H17_STAGE4_REFUSED reason=probe-node-missing'
    exit 2
}

# Rung 1: read-only ASC snapshot. Zero engine writes.
wake_rung asc-snapshot

if test "${1:-}" = GO; then
    # Rung 2: the bounded wake (confirm_wake=1). PASS is any of
    # wake-word-hello, wake-word, hello-msgs, quiet-stop.
    wake_rung confirm_wake=1
    if test -r /sys/kernel/debug/pm_genpd/pm_genpd_summary; then
        grep '^ane_sys' /sys/kernel/debug/pm_genpd/pm_genpd_summary || true
    fi
    echo 'H17_STAGE4_PASS'
fi
