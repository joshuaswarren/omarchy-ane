#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# J700 H17 first power vote. Stop at the first failed rung.
set -euo pipefail

repo=${H17_REPO:-/home/admin/omarchy-ane}
module_dir=$repo/ane/h17
boot_id=$(cat /proc/sys/kernel/random/boot_id)
log_dir=${H17_LOG_DIR:-/var/log/omarchy-ane}
mkdir -p "$log_dir"
exec > >(tee "$log_dir/h17-stage3-$boot_id.log") 2>&1

cleanup() {
    if lsmod | grep -q '^ane_h17_ps_probe '; then
        rmmod ane_h17_ps_probe
    fi
    if lsmod | grep -q '^ane_h17_adt_probe '; then
        rmmod ane_h17_adt_probe
    fi
}
trap cleanup EXIT

run_rung() {
    local stage=$1 expected=$2
    shift 2
    local before lines
    before=$(dmesg --color=never | wc -l)
    insmod "$module_dir/ane_h17_adt_probe.ko" optin=1 "$@"
    lines=$(dmesg --color=never | tail -n +"$((before + 1))")
    printf '%s\n' "$lines" | grep 'ane_h17_adt_probe'
    printf '%s\n' "$lines" | grep -Fq "RESULT stage=$stage soc=t8140 verdict=PASS reason=$expected"
    rmmod ane_h17_adt_probe
}

printf 'boot_id=%s kernel=%s repo=%s\n' "$boot_id" "$(uname -r)" "$(git -C "$repo" rev-parse HEAD)"
test "$(uname -r)" = 7.2.11-6-1-ARCH
test "$(tr -d '\000' < /proc/device-tree/model)" = 'Apple MacBook Neo'
python3 "$repo/packaging/omarchy-ane-firmware-fetch" --check
run_rung 0 segments
run_rung 1 preload-readable inspect_preload=1
run_rung 2 preload-whitelist compare_preload=1
python3 "$module_dir/preflight.py"
if ! test -e /proc/device-tree/soc/ane-h17-ps-probe/compatible; then
    echo 'H17_STAGE3_REFUSED reason=probe-node-missing'
    exit 2
fi

before=$(dmesg --color=never | wc -l)
insmod "$module_dir/ane_h17_ps_probe.ko" optin=1
lines=$(dmesg --color=never | tail -n +"$((before + 1))")
printf '%s\n' "$lines" | grep 'ane_h17_ps'
printf '%s\n' "$lines" | grep -Fq 'RESULT stage=3 verdict=PASS reason=ane-sys-on'
rmmod ane_h17_ps_probe
if test -r /sys/kernel/debug/pm_genpd/pm_genpd_summary; then
    grep '^ane_sys' /sys/kernel/debug/pm_genpd/pm_genpd_summary || true
fi
echo 'H17_STAGE3_PASS'
