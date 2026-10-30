#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# J700 H17 stage 4: bounded firmware wake. Run with no argument for the
# read-only ASC snapshot only; pass GO as the first argument to also do the
# confirm_wake=1 run (one optional child state, one RUN write, bounded
# polls, mailbox peek, RUN clear). Stop at the first failed rung. A hang
# costs one reboot. The environment overrides exist for
# test-stage4.sh; nothing needs to be set on the machine.
set -euo pipefail

repo=${H17_REPO:-/home/admin/omarchy-ane}
module_dir=$repo/ane/h17
boot_id=$(cat "${H17_BOOT_ID_FILE:-/proc/sys/kernel/random/boot_id}")
log_dir=${H17_LOG_DIR:-/var/log/omarchy-ane}
uname_r=${UNAME_R:-$(uname -r)}
model_file=${H17_MODEL_FILE:-/proc/device-tree/model}
probe_node=${H17_PROBE_NODE:-/proc/device-tree/soc/ane-h17-ps-probe/compatible}
genpd_summary=${H17_GENPD:-/sys/kernel/debug/pm_genpd/pm_genpd_summary}
insmod=${INSMOD:-insmod}
rmmod=${RMMOD:-rmmod}
dmesg=${DMESG:-dmesg}
lsmod=${LSMOD:-lsmod}
mkdir -p "$log_dir"
exec > >(tee "$log_dir/h17-stage4-$boot_id.log") 2>&1

cleanup() {
    if "$lsmod" | grep -q '^ane_h17_wake '; then
        "$rmmod" ane_h17_wake || true
    fi
}
trap cleanup EXIT

wake_rung() {
    local before lines
    before=$("$dmesg" --color=never | wc -l)
    "$insmod" "$module_dir/ane_h17_wake.ko" optin=1 "$@"
    lines=$("$dmesg" --color=never | tail -n +"$((before + 1))")
    printf '%s\n' "$lines" | grep 'ane_h17_wake'
    case "$lines" in
    *reason=core-not-stopped*)
        echo 'H17_STAGE4_CORE_NOT_STOPPED domain stays on; a reboot is required'
        exit 3
        ;;
    esac
    case "$lines" in
    *"RESULT stage=4 verdict=PASS"*) "$rmmod" ane_h17_wake ;;
    *) echo 'H17_STAGE4_NO_RESULT'; exit 1 ;;
    esac
}

printf 'boot_id=%s kernel=%s repo=%s\n' "$boot_id" "$uname_r" "$(git -C "$repo" rev-parse HEAD)"
test "$uname_r" = 7.2.11-6-1-ARCH
test "$(tr -d '\000' < "$model_file")" = 'Apple MacBook Neo'
test -z "$("$lsmod" | grep '^ane_h17_')"
(cd "$repo" && ${H17_PREFLIGHT:-python3 ane/h17/preflight.py}) >/dev/null
test -e "$probe_node" || {
    echo 'H17_STAGE4_REFUSED reason=probe-node-missing'
    exit 2
}

# Rung 1: read-only ASC snapshot. Zero engine writes.
wake_rung

if test "${1:-}" = GO; then
    # Rung 2: the bounded wake (confirm_wake=1). PASS is any of
    # wake-word-hello, wake-word, hello-msgs, quiet-stop.
    wake_rung confirm_wake=1
    if test -r "$genpd_summary"; then
        grep '^ane_sys' "$genpd_summary" || true
    fi
    echo 'H17_STAGE4_PASS'
fi
exit 0
