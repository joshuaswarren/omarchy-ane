#!/bin/bash
# pmpoot-prepare.sh — Jw16PmpOot step-1/2 offline prep ON jw16 (no reboot, no load).
# 1. build pmp_oot.ko against the RUNNING kernel's headers; verify vermagic
# 2. rebuild the proven variant DTB (pmp4-variant.sh variant == bf5bb604 bytes),
#    then apply the ONE delta: pmp node compatible -> apple,t6000-pmp-oot
# 3. prove the delta is EXACTLY the compatible property (FDT-walk diff)
# 4. re-verify the 78 tunable channels on the oot DTB, dtc parse, write the
#    oot sha into pmp4-variant.sha for the proven swap machinery
set -euo pipefail

DSTDIR=/var/tmp/ane-pmp4
MODDIR=/var/tmp/pmpoot
K=7.1.13-3-2-ARCH
PMP=/soc/pmp@28e700000
EXPECT_PLAIN=bf5bb60476cf034a77d93f7e7bdc3489acac29f03ff49a26c394c35e1c1a0ac7
mkdir -p "$MODDIR/logs" "$DSTDIR"

echo "== [1] module build =="
SRC=${PMPOOT_SRC:-$MODDIR/pmp_oot.c}
[ -f "$SRC" ] || { echo "FATAL: module source missing at $SRC"; exit 3; }
cp -f "$(dirname "$0")/Makefile" "$MODDIR/Makefile" 2>/dev/null || true
make -C "/lib/modules/$K/build" M="$MODDIR" clean >/dev/null
make -C "/lib/modules/$K/build" M="$MODDIR" modules
[ -f "$MODDIR/pmp_oot.ko" ] || { echo "FATAL: no pmp_oot.ko produced"; exit 4; }
echo "-- vermagic:"; modinfo -F vermagic "$MODDIR/pmp_oot.ko"
echo "-- running:  $(uname -r)  ($(uname -v))"
V=$(modinfo -F vermagic "$MODDIR/pmp_oot.ko")
case "$V" in "$K "*ok*) echo "VERMAGIC-PREFIX-OK";; "$K"*) echo "VERMAGIC-RELEASE-OK";; *) echo "FATAL vermagic mismatch: $V"; exit 5;; esac
sha256sum "$MODDIR/pmp_oot.ko"

echo "== [2] variant DTB rebuild + compatible delta =="
cd "$DSTDIR"
./pmp4-variant.sh variant
cp -f t6001-j316c-pmp4.dtb t6001-j316c-pmp4-plain.dtb
GOT=$(sha256sum t6001-j316c-pmp4-plain.dtb | cut -d' ' -f1)
echo "plain variant sha: $GOT (expected $EXPECT_PLAIN)"
[ "$GOT" = "$EXPECT_PLAIN" ] || { echo "FATAL: plain variant is not the proven bf5bb604 bytes"; exit 6; }

fdtput -t s t6001-j316c-pmp4.dtb "$PMP" compatible 'apple,t6000-pmp-oot'
echo "-- new compatible: [$(fdtget t6001-j316c-pmp4.dtb "$PMP" compatible)]"
[ "$(fdtget t6001-j316c-pmp4.dtb "$PMP" compatible)" = "apple,t6000-pmp-oot" ] \
	|| { echo "FATAL: compatible not applied"; exit 7; }

echo "== [3] FDT-walk diff (must list ONLY the compatible property) =="
python3 - t6001-j316c-pmp4-plain.dtb t6001-j316c-pmp4.dtb <<'PY'
import struct, sys
def props(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'\xd0\x0d\xfe\xed'
    toff = struct.unpack('>I', d[8:12])[0]
    soff = struct.unpack('>I', d[12:16])[0]
    i, out, path_stack, target = toff, {}, [], False
    while i < len(d):
        tok = struct.unpack('>I', d[i:i+4])[0]; i += 4
        if tok == 1:
            e = d.index(b'\0', i); nm = d[i:e].decode()
            i = (e + 4) & ~3
            path_stack.append(nm)
            target = ('/' + '/'.join(x for x in path_stack if x)) == '/soc/pmp@28e700000'
        elif tok == 2:
            path_stack.pop(); target = False
        elif tok == 3:
            ln, no = struct.unpack('>II', d[i:i+8]); i += 8
            e = d.index(b'\0', soff + no)
            nm = d[soff + no:e].decode()
            val = d[i:i+ln]; i = (i + ln + 3) & ~3
            if target:
                out[nm] = val
        elif tok == 9:
            break
    return out

a, b = props(sys.argv[1]), props(sys.argv[2])
assert set(a) == set(b), f"property sets differ: {set(a) ^ set(b)}"
diffs = [k for k in a if a[k] != b[k]]
if diffs != ['compatible']:
    print(f"FATAL: unexpected property diffs: {diffs}"); sys.exit(1)
print("DIFF-OK: only /soc/pmp@28e700000/compatible changed")
print("  old:", a['compatible'], "->", "new:", b['compatible'])
PY

echo "== [4] channels + parse + sha pin =="
dtc -I dtb -O null t6001-j316c-pmp4.dtb >/dev/null && echo "dtc parse OK"
# channel re-verification on the OOT dtb via direct FDT scan (variant() must
# NOT be re-run here — it would overwrite the oot DTB with plain bytes)
python3 - t6001-j316c-pmp4.dtb <<'PY'
import struct, sys, hashlib, os, glob
d = open(sys.argv[1], 'rb').read()
toff = struct.unpack('>I', d[8:12])[0]
soff = struct.unpack('>I', d[12:16])[0]
i, found, cur, depth = toff, {}, [], 0
while i < len(d):
    tok = struct.unpack('>I', d[i:i+4])[0]; i += 4
    if tok == 1:
        e = d.index(b'\0', i); cur.append(d[i:e].decode()); i = (e + 4) & ~3
    elif tok == 2:
        cur.pop()
    elif tok == 3:
        ln, no = struct.unpack('>II', d[i:i+8]); i += 8
        e = d.index(b'\0', soff + no)
        nm = d[soff + no:e].decode()
        val = d[i:i+ln]; i = (i + ln + 3) & ~3
        if [x for x in cur if x] == ['soc', 'pmp@28e700000'] and nm.startswith('apple,tunable-'):
            found[nm] = val
    elif tok == 9:
        break
want = {}
for f in sorted(glob.glob('/var/tmp/ane-pmp4/channels/*.bin')):
    want['apple,tunable-' + os.path.basename(f)[:-4]] = open(f, 'rb').read()
assert set(found) == set(want), "tunable set mismatch"
for nm, blob in want.items():
    assert found[nm] == blob, f"mismatch {nm}"
print(f"CHANNELS-OK: {len(want)} tunables byte-identical on the oot DTB")
PY
OOT=$(sha256sum t6001-j316c-pmp4.dtb | cut -d' ' -f1)
echo "$OOT" > pmp4-variant.sha
echo "oot variant sha: $OOT (pinned into pmp4-variant.sha for swap)"
echo "PREPARE-OK (no boot, no load)"
