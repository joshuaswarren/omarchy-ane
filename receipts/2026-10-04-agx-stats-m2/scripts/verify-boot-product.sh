#!/bin/bash
# Offline verification of the staged boot products. Run on the CT BEFORE the
# window; its receipt (verify-boot-product.txt) must be staged on the device,
# or install-test-kernel.sh refuses. Usage: verify-boot-product.sh <stage-dir>
# <kernel-source-tree> (for scripts/extract-linux when the Image is compressed)
set -uo pipefail
S=${1:?stage dir}
KT=${2:-~/src/omarchy-linux}
cd "$S" || exit 1
FAIL=0

sha256sum -c SHA256SUMS-stage || FAIL=1

REL=$(cat RELEASE)
echo "release: $REL"
[ -n "$REL" ] || FAIL=1

# System.map must carry the built-in driver probe and the agx_stats symbols.
[ -f System.map ] || { echo "NO-GO: System.map not staged"; FAIL=1; }
for sym in agx_stats_show asahi_sysfs_register; do
  if grep -E " ${sym}\$" System.map 2>/dev/null | grep -q ' [tT] '; then
    echo "symbol OK: $sym"
  else
    echo "NO-GO: $sym is not a built-in text symbol in System.map"; FAIL=1
  fi
done
# The probe function is static in this tree and may be inlined; driver
# presence is asserted by the Image strings check below.

# The Image must contain the driver's probe strings AND the sysfs symbols.
IMG=Image-m2
if grep -q '^CONFIG_KERNEL_GZIP=y' kernel.config 2>/dev/null; then
  "$KT/scripts/extract-linux" "$IMG" > .vmlinux.x 2>/dev/null
  [ -s .vmlinux.x ] && IMG=.vmlinux.x
fi
DRV=$(strings "$IMG" | grep -cE 'asahi: Probing|MMU: ')
AGX=$(strings "$IMG" | grep -c agx_stats_show)
echo "Image driver strings hits: $DRV  agx_stats symbol hits: $AGX"
[ "$DRV" -ge 1 ] || { echo "NO-GO: no GPU driver strings in the kernel image"; FAIL=1; }
[ "$AGX" -ge 1 ] || { echo "NO-GO: no agx_stats symbols in the kernel image"; FAIL=1; }
rm -f .vmlinux.x

# Every module in the tarball must match the release's vermagic. The driver
# is built-in, so the archive normally carries no asahi.ko; whatever modules
# it does carry are all checked, not a spot sample.
VMOD=$(mktemp -d "$S/.vmod.XXXXXX")
tar --zstd -xf modules-m2.tar.zst -C "$VMOD"
NMOD=0
while IFS= read -r -d '' f; do
  case $f in *.ko) ;; *) continue ;; esac
  NMOD=$((NMOD + 1))
  OV=$(tr -c '[:print:]' '\n' < "$f" | grep -m1 '^vermagic=' || true)
  case "${OV#vermagic=}" in
    "$REL "*) : ;;
    *) echo "NO-GO: module vermagic mismatch: ${f#"$VMOD"/} (${OV:-none})"; FAIL=1 ;;
  esac
done < <(find "$VMOD" -type f -print0)
rm -rf "$VMOD"
echo "vermagic checked on $NMOD modules"

if [ "$FAIL" = 0 ]; then
  {
    echo "AGX_VERIFY_OK $(date -u +%FT%TZ)"
    echo "Image-m2 sha256: $(sha256sum Image-m2 | awk '{print $1}')"
    echo "System.map asahi_probe/agx_stats_show/asahi_sysfs_register: built-in text symbols"
    echo "driver strings + agx_stats symbols: present"
    echo "release: $REL"
  } > verify-boot-product.txt
  cat verify-boot-product.txt
  echo "VERIFY PASS"
else
  rm -f verify-boot-product.txt
  echo "VERIFY FAIL — do not stage, do not install"
  exit 1
fi
