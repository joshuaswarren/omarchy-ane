#!/bin/bash
# pmpoot3-closeout.sh — STUDIO-SIDE close-out for the PmpOot3 window (either
# outcome): revert to the standard chain, reboot, verify the full close-out
# gate (A4 pattern), restore llm + timer, drop the maintenance lines, close
# the watcher. Run ONLY after the cycle returned (or the box was recovered).
set -uo pipefail
JW16=jw16mbp1-linux
K=7.1.13-3-2-ARCH
W=/tmp/pmpoot3-watch-home
LABDIR="$W/.local/share/apple-silicon-lab/artifacts/Jw16PmpOot3/watcher"
STD_BOOT=6e8f90c895d83d9f1a91a0a067f45a0190e300e6a9a018e02c39851578b21d01
STD_DTB=7b6ac97aa67de56489b50a172a8c9f769bc9f4cf97d69a65dfc3587c34331f28

fail(){ echo "FATAL: $*"; exit 30; }
rec(){ echo "[closeout $(date -u +%FT%TZ)] $*"; }

echo "== [1] revert standard chain =="
ssh "$JW16" 'bash /var/tmp/ane-pmp4/pmp4-variant.sh revert' | tee /tmp/pmpoot3-revert.log
grep -q "reverted" /tmp/pmpoot3-revert.log || fail "revert did not confirm"
ssh "$JW16" "sudo sha256sum /boot/efi/m1n1/boot.bin /var/lib/omarchy-ane/dtbs/$K/t6001-j316c.dtb" | tee /tmp/pmpoot3-revert-shas.log
grep -q "$STD_BOOT" /tmp/pmpoot3-revert-shas.log || fail "boot.bin not standard after revert"
grep -q "$STD_DTB" /tmp/pmpoot3-revert-shas.log || fail "DTB not standard after revert"

echo "== [2] reboot =="
ssh -o ConnectTimeout=8 "$JW16" 'sudo systemctl reboot'
UP=""
for i in $(seq 1 36); do
  sleep 10
  if ssh -o ConnectTimeout=4 -o BatchMode=yes "$JW16" true 2>/dev/null; then UP=1; echo "ssh up after ~$((i*10))s"; break; fi
done
[ -n "$UP" ] || fail "no ssh within 360 s after close-out reboot"
sleep 8

echo "== [3] verify standard boot =="
VBOOT=$(ssh "$JW16" 'cat /proc/sys/kernel/random/boot_id')
rec "boot id: $VBOOT"
[ "$(ssh "$JW16" 'uname -r')" = "$K" ] || fail "not the stock kernel"
ANEINFO=$(ssh "$JW16" 'for f in /sys/bus/platform/devices/*ane*/driver; do [ -e "$f" ] && basename "$(readlink -f "$f")"; done | sort -u | tr "\n" " "; readlink -f /sys/bus/platform/devices/*ane*/iommu 2>/dev/null; sudo dmesg | grep -c "DART containment armed" || true')
rec "ane bind/iommu/containment: $ANEINFO"
echo "$ANEINFO" | grep -q "^ane " || fail "ane driver not bound"
echo "$ANEINFO" | grep -q "apple-dart" || fail "ane iommu is not apple-dart"
OOPS=$(ssh "$JW16" 'sudo dmesg | grep -Ec "Oops|BUG:|kernel panic" || true')
[ "$OOPS" = "0" ] || fail "oops on the close-out boot: $OOPS"
FAILED=$(ssh "$JW16" 'systemctl --failed --no-legend | wc -l')
[ "$FAILED" = "0" ] || rec "WARN: failed units $FAILED"
BTRFS=$(ssh "$JW16" 'sudo btrfs device stats / | grep -vc " 0$" || true')
[ "$BTRFS" = "0" ] || fail "btrfs device stats nonzero errors"
rec "btrfs device stats clean"

echo "== [4] n1 smoke inside the gpu window =="
ssh "$JW16" 'sudo systemctl stop llm-inference; sleep 2'
ssh "$JW16" "flock -w 300 /tmp/m1-gpu.lock bash -c 'exec 9>/tmp/m1-gpu.lock; sudo /var/tmp/ane-perf/gap7-bench.sh /var/tmp/pmpoot3/logs/closeout-smoke 1 8'" | tee /tmp/pmpoot3-smoke.log
grep -q "hidden=fca96f1355485ec3" /tmp/pmpoot3-smoke.log || fail "n1/n8 smoke not bit-exact"
rec "smoke bit-exact PASS"

echo "== [5] llm-inference restore + verify =="
ssh "$JW16" 'sudo systemctl start llm-inference; sleep 5; systemctl is-active llm-inference'
HEALTH=$(ssh "$JW16" 'KEY=$(sudo cat /etc/llm-inference/api-key); PORT=$(systemctl cat llm-inference | grep -o "\-\-port [0-9]*" | awk "{print \$2}"); curl -s -o /dev/null -w "%{http_code}" -H "Authorization: Bearer $KEY" "http://127.0.0.1:$PORT/health"')
[ "$HEALTH" = "200" ] || fail "llm health not 200: $HEALTH"
rec "llm health 200"
COMP=$(ssh "$JW16" 'KEY=$(sudo cat /etc/llm-inference/api-key); PORT=$(systemctl cat llm-inference | grep -o "\-\-port [0-9]*" | awk "{print \$2}"); curl -s -H "Authorization: Bearer $KEY" -H "Content-Type: application/json" -d "{\"model\":\"default\",\"messages\":[{\"role\":\"user\",\"content\":\"ping\"}],\"max_tokens\":1}" "http://127.0.0.1:$PORT/v1/chat/completions" | head -c 400')
echo "$COMP" | grep -q "finish_reason" || fail "authenticated completion failed: $COMP"
rec "authenticated completion OK: $(echo "$COMP" | tr -d "\n" | head -c 120)"

echo "== [6] venv, ICD, timer =="
VENV=$(ssh "$JW16" '/var/tmp/v072-venv-fused/bin/pip show mlx-omarchy 2>/dev/null | grep -i ^version')
rec "serving venv: $VENV"
echo "$VENV" | grep -q "0.32.4.dev202610012238+0aa148382" || rec "WARN: venv version unexpected"
ICD=$(ssh "$JW16" 'grep -rl "libvulkan_asahi.so.1432df0196-transfer" /usr/share/vulkan/icd.d/*.json 2>/dev/null | head -3')
[ -n "$ICD" ] || fail "ICD json does not point at libvulkan_asahi.so.1432df0196-transfer"
rec "ICD json -> libvulkan_asahi.so.1432df0196-transfer: $ICD"
ssh "$JW16" 'sudo systemctl start llm-benchmark-recovery.timer; systemctl is-active llm-benchmark-recovery.timer'
rec "benchmark timer re-armed"

echo "== [7] markers + watcher close =="
ssh "$JW16" "sudo sed -i '/PmpOot3 2026-10-02T01:55Z/d' /var/tmp/JW16_MAINTENANCE 2>/dev/null; cat /var/tmp/JW16_MAINTENANCE 2>/dev/null || true"
sudo sed -i '/PmpOot3 2026-10-02T01:55Z/d' /var/tmp/JW16_MAINTENANCE
touch "$LABDIR/closed"
rec "maintenance lines removed; watcher close marker dropped"
rec "CLOSE-OUT-OK boot=$VBOOT"
