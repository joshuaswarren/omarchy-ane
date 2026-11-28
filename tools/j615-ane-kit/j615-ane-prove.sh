#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Prove the in-tree Aurora main T8122 (J615 / Apple M3) ANE through omarchy-ane userspace.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OMARCHY_ANE="$(cd "$SCRIPT_DIR/../.." && pwd)"
HWX_IN=""
REPEAT=100
DEV=0
RAW_HWX=0
DRY_RUN=0

usage() {
	cat <<'EOF'
usage: j615-ane-prove.sh [--omarchy-ane DIR] [--hwx FILE] [--repeat N] [--dev N] [--raw-hwx] [--dry-run]
EOF
}

while [[ $# -gt 0 ]]; do
	case "$1" in
	--omarchy-ane) OMARCHY_ANE="$2"; shift 2 ;;
	--hwx) HWX_IN="$2"; shift 2 ;;
	--repeat) REPEAT="$2"; shift 2 ;;
	--dev) DEV="$2"; shift 2 ;;
	--raw-hwx) RAW_HWX=1; shift ;;
	--dry-run) DRY_RUN=1; shift ;;
	-h|--help) usage; exit 0 ;;
	*) usage >&2; exit 2 ;;
	esac
done

OMARCHY_ANE="$(cd "$OMARCHY_ANE" && pwd)"
if [[ -z "$HWX_IN" ]]; then
	HWX_IN="$OMARCHY_ANE/fixtures/h15-hwx/add/model.hwx"
fi
if [[ ! -f "$HWX_IN" ]]; then
	echo "FAIL: HWX file not found: $HWX_IN" >&2
	exit 1
fi

WORK="$(mktemp -d -t j615-ane-prove.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

echo "=== 1. Kernel, DT and /dev/accel preflight ==="
KVER="$(uname -r)"
echo "kernel: $KVER"

DT_ANE="/proc/device-tree/soc/ane@310000000"
if [[ "$DRY_RUN" -eq 0 ]]; then
	if [[ -d "$DT_ANE" ]]; then
		echo "dt status: $(tr -d '\0' < "$DT_ANE/status" 2>/dev/null || echo missing)"
		echo "dt compatible: $(tr '\0' ',' < "$DT_ANE/compatible" 2>/dev/null | sed 's/,$//')"
		if [[ ! -f "$DT_ANE/memory-region" ]]; then
			echo "FAIL: $DT_ANE/memory-region missing (loader must inject the two no-map reserved-memory regions)" >&2
			exit 1
		fi
		echo "dt memory-region: present ($(wc -c < "$DT_ANE/memory-region") B)"
	else
		echo "WARN: $DT_ANE not present; checking /dev/accel directly"
	fi

	python3 - "$DEV" <<'PY'
import fcntl, glob, os, struct, sys

dev_want = int(sys.argv[1])
GET_CAPS = struct.Struct("<8I")
IOCTL_GET_CAPS = (3 << 30) | (GET_CAPS.size << 16) | (ord("d") << 8) | 0x40
found = 0
for path in sorted(glob.glob("/dev/accel/accel*")):
    try:
        fd = os.open(path, os.O_RDWR | os.O_CLOEXEC)
    except OSError as e:
        print(f"WARN: open({path}): {e.strerror}")
        continue
    caps = bytearray(GET_CAPS.size)
    try:
        fcntl.ioctl(fd, IOCTL_GET_CAPS, caps)
    except OSError:
        os.close(fd)
        continue
    _, sz, abi, family, sec_sz, bind_sz, io_sz, _ = GET_CAPS.unpack(caps)
    os.close(fd)
    if found == dev_want:
        print(f"device: {path} (abi={abi}, chip_family={family}, section_size={sec_sz}, exec_io_size={io_sz})")
        if abi != 3 or family != 15:
            print(f"FAIL: expected abi=3 chip_family=15 (T8122/H15), got abi={abi} chip_family={family}", file=sys.stderr)
            sys.exit(1)
        sys.exit(0)
    found += 1
print(f"FAIL: no ANE /dev/accel/accel* device at index {dev_want} (check dmesg | grep -i ane)", file=sys.stderr)
sys.exit(1)
PY
else
	echo "dry-run: skipping DT and /dev/accel probe"
fi

echo "=== 2. Build omarchy-ane libane + ane-run ==="
make -C "$OMARCHY_ANE/libane" libane >/dev/null
make -C "$OMARCHY_ANE/tools" ane-run >/dev/null
RUNNER="$OMARCHY_ANE/tools/ane-run"
echo "runner: $RUNNER"

echo "=== 3. Prepare H15 add-128 HWX and fp16 test vectors ==="
HWX_RUN="$WORK/model-h15.hwx"
python3 - "$HWX_IN" "$HWX_RUN" "$WORK" "$RAW_HWX" <<'PY'
import hashlib, pathlib, struct, sys

hwx_in = pathlib.Path(sys.argv[1]).read_bytes()
hwx_out = pathlib.Path(sys.argv[2])
work = pathlib.Path(sys.argv[3])
raw_mode = int(sys.argv[4])

def canonicalize_apple_h15(b):
    """Strip 3 non-executing H15 metadata items rejected by Aurora ane_h16_hwx.c:
    1) __TEXT SG_NORELOC flag (4 -> 0) when no __DATA scratch segment exists;
    2) LC_NOTE (0x31) 'src model info';
    3) Debug SYMTAB stabs (0x20 / 0x80), keeping only the N_SECT surface symbols.
    Task stream (__TEXT,__text), __TEXT,__const, 0x890 program descriptor, and
    all tensor descriptors remain byte-identical.
    """
    hdr = list(struct.unpack_from("<8I", b, 0))
    if hdr[0] != 0xbeefface or hdr[1] != 0x80 or hdr[2] != 6:
        raise ValueError("input is not an H15 (subtype 6) HWX")
    ncmds = hdr[4]
    pos = 32
    cmds = []
    symtab = None
    has_scratch = False
    for _ in range(ncmds):
        cmd, sz = struct.unpack_from("<II", b, pos)
        chunk = bytearray(b[pos:pos + sz])
        if cmd == 0x19:
            seg = chunk[8:24].split(b"\0", 1)[0].decode()
            if seg == "__DATA":
                has_scratch = True
        cmds.append((cmd, chunk))
        pos += sz

    out_cmds = []
    new_sym_idx = None
    for cmd, chunk in cmds:
        if cmd == 0x31:
            continue
        if cmd == 0x19:
            seg = chunk[8:24].split(b"\0", 1)[0].decode()
            if seg == "__TEXT" and not has_scratch and struct.unpack_from("<I", chunk, 68)[0] == 4:
                struct.pack_into("<I", chunk, 68, 0)
        if cmd == 2:
            symtab = struct.unpack_from("<4I", chunk, 8)
            new_sym_idx = len(out_cmds)
        out_cmds.append(chunk)

    if symtab is None:
        raise ValueError("HWX missing LC_SYMTAB")
    symoff, nsyms, stroff, strsize = symtab
    kept_syms = bytearray()
    kept_strs = bytearray(b"\x00")
    kept_count = 0
    for i in range(nsyms):
        s = b[symoff + i * 16:symoff + (i + 1) * 16]
        strx = struct.unpack_from("<I", s, 0)[0]
        ntype, nsect = s[4], s[5]
        if nsect == 0 or (ntype & ~1) != 0xe:
            continue
        name = b[stroff + strx:stroff + strsize].split(b"\0", 1)[0]
        new_strx = len(kept_strs)
        kept_strs += name + b"\x00"
        kept_syms += struct.pack("<IBBHQ", new_strx, ntype, nsect,
                                 struct.unpack_from("<H", s, 6)[0],
                                 struct.unpack_from("<Q", s, 8)[0])
        kept_count += 1
    while len(kept_strs) % 8:
        kept_strs.append(0)

    sizeofcmds = sum(len(c) for c in out_cmds)
    new_symoff = (32 + sizeofcmds + 15) & ~15
    new_stroff = new_symoff + len(kept_syms)
    struct.pack_into("<4I", out_cmds[new_sym_idx], 8,
                     new_symoff, kept_count, new_stroff, len(kept_strs))

    out = bytearray(b)
    first_file_off = min(
        struct.unpack_from("<Q", c, 40)[0]
        for c in out_cmds
        if struct.unpack_from("<I", c, 0)[0] == 0x19 and struct.unpack_from("<Q", c, 48)[0] > 0
    )
    if new_stroff + len(kept_strs) > first_file_off:
        raise ValueError("canonicalized headers exceed first segment file offset")
    out[:first_file_off] = b"\x00" * first_file_off
    hdr[4] = len(out_cmds)
    hdr[5] = sizeofcmds
    struct.pack_into("<8I", out, 0, *hdr)
    p = 32
    for c in out_cmds:
        out[p:p + len(c)] = c
        p += len(c)
    out[new_symoff:new_symoff + len(kept_syms)] = kept_syms
    out[new_stroff:new_stroff + len(kept_strs)] = kept_strs
    return bytes(out)

hwx_final = hwx_in if raw_mode else canonicalize_apple_h15(hwx_in)
hwx_out.write_bytes(hwx_final)
print(f"hwx_in_sha256: {hashlib.sha256(hwx_in).hexdigest()} ({len(hwx_in)} B)")
print(f"hwx_run_sha256: {hashlib.sha256(hwx_final).hexdigest()} ({len(hwx_final)} B, raw={raw_mode})")

# Deterministic add-128 [1, 1, 128, 128] fp16 inputs and golden output
a = [(i % 17 - 8) * 0.125 for i in range(16384)]
b = [(i % 11 - 5) * 0.25 for i in range(16384)]
exp = [x + y for x, y in zip(a, b)]
a_bytes = b"".join(struct.pack("<e", x) for x in a)
b_bytes = b"".join(struct.pack("<e", x) for x in b)
e_bytes = b"".join(struct.pack("<e", x) for x in exp)
assert hashlib.sha256(e_bytes).hexdigest() == "d113d9ae6734b7a7e233458229f64f382da99a3f93868eef3abb06a29f7f4ce8"
(work / "a.bin").write_bytes(a_bytes)
(work / "b.bin").write_bytes(b_bytes)
(work / "expected.bin").write_bytes(e_bytes)
print("vectors: 16384 fp16 elements (32768 B/surface), golden d113d9ae6734b7a7e233458229f64f382da99a3f93868eef3abb06a29f7f4ce8")
PY

if [[ "$DRY_RUN" -eq 1 ]]; then
	echo "dry-run: skipping device execution (HWX and vectors staged cleanly)"
	exit 0
fi

echo "=== 4. Execute through omarchy-ane ane-run (--repeat $REPEAT) ==="
OUT_Y="$WORK/y.bin"
RAW_NS="$WORK/lat.ns"
RUN_OUT="$("$RUNNER" --hwx "$HWX_RUN" --dev "$DEV" \
	--in "0=$WORK/a.bin" --in "1=$WORK/b.bin" --out "0=$OUT_Y" \
	--repeat "$REPEAT" --time-raw "$RAW_NS" --check add-dense)"
echo "$RUN_OUT"

GOT_SHA="$(sha256sum "$OUT_Y" | awk '{print $1}')"
WANT_SHA="d113d9ae6734b7a7e233458229f64f382da99a3f93868eef3abb06a29f7f4ce8"
echo "output_sha256: $GOT_SHA"
if [[ "$GOT_SHA" != "$WANT_SHA" ]]; then
	echo "FAIL: output SHA-256 mismatch (want $WANT_SHA)" >&2
	exit 1
fi

echo "=== PASS: J615 T8122 in-tree ANE verified ==="
echo "SUMMARY: kernel=$KVER repeat=$REPEAT sha256=$GOT_SHA $(echo "$RUN_OUT" | grep '^exec ms')"
