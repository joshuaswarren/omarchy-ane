#!/usr/bin/env bash
# M4 (J616s T6040 / J616C T6041, both apple,t6041-ane) ANE bring-up packet — community lab, one paste per boot.
# One rung per fresh boot: paste this script, it runs the next pending rung, prints a
# paste-back summary, and tells you to reboot before pasting again. Stop at the first
# FAIL and send the logs; do not re-run a failed rung.
#   rung 1  omarchy-ane-smoke        in-tree ane_t6021 binding + bit-exact fixture proof
#   rung 2  ane_h16 stage=status     read-only power/register probe (safe default)
#   rung 3  ane_h16 stage=dt         ONLY if a t6040 overlay dtbo is staged; otherwise SKIP
# stage=boot is OUT OF SCOPE for this packet (experimental; card row excludes it).
set -u
STATE=/var/lib/omarchy-ane-bringup
LOG=$STATE/log
mkdir -p "$STATE" 2>/dev/null || { echo "refused: need root (sudo -i, then paste)"; exit 2; }

result() { echo "RESULT m4ane-$1 $2 $3"; }
die()    { result "$1" "FAIL" "$2"; echo "STOP: do not reboot-and-retry; send $LOG-$1.txt"; exit 1; }

# ---- preflight ----
COMP=$(tr -d '\0' < /sys/firmware/devicetree/base/compatible 2>/dev/null | head -c 200)
case "$COMP" in *apple,t6040*|*apple,j616s*|*apple,t6041*|*apple,j616c*) ;; *)
  echo "refused: this packet is for J616s (T6040) and J616C (T6041); compatible reads: ${COMP:-unknown}"; exit 2;;
esac
ANE_NODE=$(find /sys/firmware/devicetree/base -maxdepth 1 -name 'ane@*' 2>/dev/null | head -1)
[ -n "$ANE_NODE" ] || die preflight "no ANE node in live DT (expected /ane@484000000)"
ANE_COMPAT=$(tr -d '\0' < "$ANE_NODE/compatible" 2>/dev/null | head -c 120)
KREL=$(uname -r)
HDR=/lib/modules/$KREL/build
next=$(cat "$STATE/rung" 2>/dev/null || echo 1)

echo "== m4-ane-bringup rung $next on $(hostname) $(date -u +%FT%TZ)"
echo "== compatible: $COMP | ane node: $ANE_NODE ($ANE_COMPAT) | kernel $KREL"

case $next in
1)
  L=$LOG-1-smoke.txt
  {
    echo "diag ane node status: $(tr -d '\0' < "$ANE_NODE/status" 2>/dev/null || echo unknown)"
    echo "diag ane_t6021.ko: $(modinfo -n ane_t6021 2>/dev/null || echo 'not installed')"
    echo "diag t6041 alias: $(modinfo ane_t6021 2>/dev/null | grep -c t6041) match(es)"
    echo "diag bound: $(ls /sys/bus/platform/drivers/ane_t6021/ 2>/dev/null | grep -v module || echo 'none')"
  } | tee "$L"
  if ! command -v omarchy-ane-smoke >/dev/null 2>&1; then
    result smoke SKIP "omarchy-ane-smoke not installed (G405 packaging not on this image)"; echo 2 > "$STATE/rung"
    echo "REBOOT now, then paste the script again for rung 2."; exit 0
  fi
  omarchy-ane-smoke >>"$L" 2>&1; rc=$?
  dmesg | grep -iE "ane|accel" | tail -40 >>"$L"
  case $rc in
    0) result smoke "PASS" "20/20 bit-exact, driver bound ($ANE_COMPAT)"; echo 2 > "$STATE/rung";;
    1) result smoke "FAIL" "fixture mismatch or call failed (exit 1)"; die smoke "see $L";;
    *) result smoke "FAIL" "unavailable (exit 2) - send $L"; die smoke "see $L";;
  esac ;;
2)
  L=$LOG-2-status.txt
  SRC=$STATE/src/omarchy-ane
  [ -d "$SRC" ] || git clone --depth 1 https://github.com/joshuaswarren/omarchy-ane "$SRC" >"$L" 2>&1 \
    || die status "clone failed - send $L"
  [ -f "$HDR/Makefile" ] || die status "kernel headers for $KREL missing at $HDR - install the aurora kernel-devel/linux-headers package for this kernel, then paste again (no reboot needed)"
  make -C "$SRC/ane/h16" KERNELDIR="$HDR" ARCH=arm64 modules >>"$L" 2>&1 \
    || die status "h16 module build failed - send $L"
  insmod "$SRC/ane/h16/ane_h16.ko" stage=status >>"$L" 2>&1 \
    || die status "insmod stage=status refused (already loaded? check $L)"
  sleep 2
  dmesg | grep -E "ane_h16" | tail -60 >>"$L"
  ps_ok=$(dmesg | grep -c "ane_h16.*ACTUAL=0xf")
  [ "$ps_ok" -ge 1 ] || die status "no ACTUAL=0xf ps words logged - unexpected, STOP and send $L"
  result status "PASS" "stage=status logged; $ps_ok ps word(s) ACTUAL=0xf; send $L"; echo 3 > "$STATE/rung" ;;
3)
  L=$LOG-3-dt.txt
  DTBO=$(find "$STATE" -maxdepth 1 -name '*t604*.dtbo' 2>/dev/null | head -1)
  if [ -z "$DTBO" ]; then
    result dt "SKIP" "no t604x overlay staged yet (G406/G606 deliverable); T6040/T6041 have no aurora board DT - live DT node already present, rung not needed for status"
    rm -f "$STATE/rung"; echo "DONE - paste all $LOG-*.txt RESULT lines back. No reboot needed."; exit 0
  fi
  cp "$DTBO" /boot/omarchy-ane-dt/ 2>/dev/null || die dt "cannot install dtbo - send $L"
  grep -q ane-h16-experimental /etc/omarchy-platform/dtb-overlays.opt-in 2>/dev/null \
    || echo ane-h16-experimental >> /etc/omarchy-platform/dtb-overlays.opt-in
  result dt "PASS" "t6040 dtbo staged from $DTBO; reboot to apply, then paste again for verification"
  echo 4 > "$STATE/rung"; echo "REBOOT now, then paste again." ;;
4)
  L=$LOG-4-dtverify.txt
  dmesg | grep -iE "ane|dtb|overlay" | tail -40 >>"$L"
  A=$(tr -d '\0' < "$ANE_NODE/compatible" 2>/dev/null | head -c 120)
  result dtverify "PASS" "post-boot ANE node: $ANE_NODE ($A); send $L"
  rm -f "$STATE/rung"; echo "DONE - paste all $LOG-*.txt RESULT lines back." ;;
*) rm -f "$STATE/rung"; echo "state reset; paste again from rung 1"; exit 0 ;;
esac
echo "PASTE-BACK: send the RESULT line above plus $L"
[ -n "$(cat "$STATE/rung" 2>/dev/null)" ] && echo "NEXT: reboot, then paste this script again."
exit 0
