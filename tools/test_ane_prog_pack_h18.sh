#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren
#
# test_ane_prog_pack_h18.sh: packs the checked-in H18 add program (fixtures/h18-anec/add, compiled on an M6) with
# ane-prog-pack --chip h18 and checks the program file the ane/h18 module reads: header, the six sections ids, the
# three buffers with sizes and directions, the input data placement, that the default (H14) role set still refuses the
# H18 stream, and that the derived BAR slot to tag table is the documented one. Needs cc and python3.
# usage: sh tools/test_ane_prog_pack_h18.sh   (from the repository root)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/stub"
printf '#include <linux/ioctl.h>\n#include <linux/types.h>\n#define DRM_IOCTL_BASE 0x64\n#define DRM_COMMAND_BASE 0x40\n#define DRM_IO(nr) _IO(DRM_IOCTL_BASE, nr)\n#define DRM_IOR(nr, t) _IOR(DRM_IOCTL_BASE, nr, t)\n#define DRM_IOW(nr, t) _IOW(DRM_IOCTL_BASE, nr, t)\n#define DRM_IOWR(nr, t) _IOWR(DRM_IOCTL_BASE, nr, t)\n' > "$tmp/stub/drm.h"
cc -O1 -I "$tmp/stub" -I "$root/libane" -I "$root/ane/src/uapi/drm" -o "$tmp/pack" "$here/ane-prog-pack.c" "$root/libane/ane_m2.c" "$root/libane/ane_sha256.c" 2> "$tmp/cc.err" ||
  { cat "$tmp/cc.err" >&2; echo "FAIL cannot build ane-prog-pack" >&2; exit 1; }
cat > "$tmp/refs.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ane.h"
#include "ane_m2.h"
int main(int argc, char **argv)
{
	FILE *f = fopen(argv[1], "rb");
	long n;
	unsigned char *buf;
	struct ane_m2_model m;
	struct ane_m2_sections s;
	unsigned j;

	ane_m2_set_chip(ANE_M2_CHIP_H18);
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc(n);
	if (fread(buf, 1, n, f) != (size_t)n)
		return 3;
	memset(&m, 0, sizeof(m));
	memset(&s, 0, sizeof(s));
	if (ane_m2_program_build(buf, n, &m, &s))
		return 1;
	for (j = 0; j < m.call_ref_count[0]; j++)
		printf("%u:%u\n", m.call_refs[0][j].slot, m.call_refs[0][j].tag);
	return 0;
}
EOF
cc -O1 -I "$tmp/stub" -I "$root/libane" -I "$root/ane/src/uapi/drm" -o "$tmp/refs" "$tmp/refs.c" "$root/libane/ane_m2.c" "$root/libane/ane_sha256.c" 2> "$tmp/cc2.err" ||
  { cat "$tmp/cc2.err" >&2; echo "FAIL cannot build the ref dumper" >&2; exit 1; }
fx="$root/fixtures/h18-anec/add"
fails=0
if "$tmp/pack" --anec "$fx/program-0.anec" -o "$tmp/x.bin" > /dev/null 2>&1; then echo "FAIL the H14 role set accepted the H18 stream"; fails=1; fi
"$tmp/pack" --chip h18 --anec "$fx/program-0.anec" --in 5="$fx/a.phys4096" --in 6="$fx/b.phys4096" -o "$tmp/prog.bin" > "$tmp/pack.out"
refs=$("$tmp/refs" "$fx/program-0.anec" | tr '\n' ' ')
if [ "$refs" != "4:2 5:5 6:6 7:4 " ]; then echo "FAIL derived slot:tag table is '$refs'"; fails=1; fi
python3 - "$tmp" "$fx" <<'EOF' || fails=1
import struct, sys
tmp, fx = sys.argv[1], sys.argv[2]
d = open(tmp + "/prog.bin", "rb").read()
magic, ver, nsec, nio, size, _ = struct.unpack_from("<IIIIQQ", d)
ok = True
def check(cond, what):
    global ok
    print(("  PASS " if cond else "  FAIL ") + what)
    ok = ok and cond
check(magic == 0x50454e41 and ver == 1 and size == len(d), "header ANEP v1 and the file size")
off = 32
ids = []
for _ in range(nsec):
    sid, z, sz, o, r = struct.unpack_from("<IIQQQ", d, off)
    off += 32
    ids.append(sid)
    check(o % 16 == 0 and o + sz <= len(d) and sz > 0, "section %d inside the file, 16 byte aligned" % sid)
check(ids == [1, 2, 3, 4, 5, 7], "section ids 1, 2, 3, 4, 5 and 7")
bufs = {}
for _ in range(nio):
    bid, dr, sz, o, ds = struct.unpack_from("<IIQQQ", d, off)
    off += 32
    bufs[bid] = (dr, sz, o, ds)
check(sorted(bufs) == [4, 5, 6], "buffers 4, 5 and 6")
check(bufs[4][0] == 1 and bufs[5][0] == 0 and bufs[6][0] == 0, "buffer 4 output, 5 and 6 inputs")
check(all(b[1] == 16384 for b in bufs.values()), "each buffer is one 16 KiB tile")
for bid, name in ((5, "a"), (6, "b")):
    want = open(fx + "/" + name + ".phys4096", "rb").read()
    check(bufs[bid][3] == len(want) and d[bufs[bid][2]:bufs[bid][2] + len(want)] == want, "buffer %d carries %s.phys4096" % (bid, name))
check(bufs[4][3] == 0, "output buffer is zero filled")
sys.exit(0 if ok else 1)
EOF
if [ "$fails" = 0 ]; then echo "test_ane_prog_pack_h18: all checks passed"; else echo "test_ane_prog_pack_h18: FAILED" >&2; exit 1; fi
