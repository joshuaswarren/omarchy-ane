#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Read-only scan of the boot ADT for the Neural Engine nodes. It loads no module, touches no ANE register and writes no file.

Usage: ane_adt_scan.py [ADT.bin]

With no argument it reads the ADT from the one MTD device named "adt" (the m1n1 phram copy of the boot device tree):
/sys/class/mtd/mtdN/name == adt, device /dev/mtdNro, opened read only. Root is needed for the device read only.
With a file argument it reads that file (an ADT dump of the same boot).

It prints, for every node whose name or compatible names the ANE: the path, the property NAMES, and the values of a short
list of properties (compatible, device_type, reg, segment-ranges, power-gates, clock-gates, clock-ids, pre-loaded). Every
other property value, including any serial number, identifier or address, is never printed. Under /chosen it prints
chip-id and board-id only.

segment-ranges holds the firmware iBoot preloaded for the ANE (two 32-byte little-endian records: physical address,
iova offset, remap iova, size | flags << 32). A boot with no ANE node or no segment-ranges has no preloaded firmware
for the ane_h15 and ane_h16 stage=boot paths to map.

Exit status: 0 an ANE node was found, 1 the ADT parsed and has no ANE node, 2 usage or a malformed ADT.
"""
import os
import re
import struct
import sys

PROPS_MAX = 4096
DEPTH_MAX = 16
READ_MAX = 16 << 20
SHOWN = ("compatible", "device_type", "reg", "segment-ranges", "power-gates", "clock-gates", "clock-ids", "pre-loaded")
CHOSEN_SHOWN = ("chip-id", "board-id")
ANE_WORD = re.compile(r"(^|[-_,])ane\d*($|[-_,])")
MTD_DIR = re.compile(r"mtd\d+")


class ADTError(Exception):
    pass


def parse(b, off=0, depth=0):
    """Walk the flattened ADT. Every length is bounded by the buffer. Returns (node, end offset)."""
    if depth > DEPTH_MAX:
        raise ADTError("nesting too deep")
    if off + 8 > len(b):
        raise ADTError("truncated")
    nprops, nchild = struct.unpack_from("<II", b, off)
    off += 8
    if nprops > PROPS_MAX or nchild > PROPS_MAX:
        raise ADTError("implausible node counts")
    props = {}
    for _ in range(nprops):
        if off + 36 > len(b):
            raise ADTError("property header truncated")
        name = b[off:off + 32].split(b"\0")[0].decode("latin1")
        size = struct.unpack_from("<I", b, off + 32)[0] & 0x7fffffff
        off += 36
        if off + size > len(b):
            raise ADTError("property data truncated")
        props[name] = b[off:off + size]
        off += (size + 3) & ~3
    kids = []
    for _ in range(nchild):
        kid, off = parse(b, off, depth + 1)
        kids.append(kid)
    return {"props": props, "kids": kids}, off


def node_name(node):
    return node["props"].get("name", b"").split(b"\0")[0].decode("latin1")


def strings(raw):
    return [s.decode("latin1") for s in raw.split(b"\0") if s]


def is_ane(node):
    return bool(ANE_WORD.search(node_name(node))) or any(ANE_WORD.search(c) for c in strings(node["props"].get("compatible", b"")))


def walk(node, path=""):
    yield path or "/", node
    for kid in node["kids"]:
        yield from walk(kid, f"{path}/{node_name(kid)}")


def show_value(name, raw):
    """One printable line per value. No property outside SHOWN reaches this function."""
    if name in ("compatible", "device_type"):
        return [", ".join(strings(raw))]
    if name == "reg" and len(raw) % 16 == 0 and raw:
        return [f"reg[{i}] = {a:#x} +{s:#x}" for i, (a, s) in enumerate(struct.iter_unpack("<QQ", raw))]
    if name == "segment-ranges" and len(raw) % 32 == 0 and raw:
        out = []
        for i, (phys, off, remap, size_flags) in enumerate(struct.iter_unpack("<QQQQ", raw)):
            out.append(f"record {i}: phys={phys:#x} iova_offset={off:#x} remap={remap:#x} size={size_flags & 0xffffffff:#x} flags={size_flags >> 32:#x}")
        return out
    if len(raw) % 4 == 0 and 0 < len(raw) <= 64:
        return ["words " + " ".join(f"{w:#x}" for w in struct.unpack(f"<{len(raw) // 4}I", raw))]
    return [f"{len(raw)} bytes: " + raw[:64].hex() + (" ..." if len(raw) > 64 else "")]


def scan(adt, out):
    root, _ = parse(adt)
    found = segments = 0
    for path, node in walk(root):
        if path == "/chosen":
            for name in CHOSEN_SHOWN:
                if name in node["props"]:
                    out(f"/chosen {name}: " + " ".join(show_value(name, node["props"][name])))
        if not is_ane(node):
            continue
        found += 1
        out(f"node {path}")
        out("  properties: " + ", ".join(sorted(node["props"])))
        for name in SHOWN:
            if name in node["props"]:
                lines = show_value(name, node["props"][name])
                out(f"  {name}: {len(lines)} lines" if name in ("reg", "segment-ranges") else f"  {name}: {lines[0]}")
                if name in ("reg", "segment-ranges"):
                    for line in lines:
                        out(f"    {line}")
                if name == "segment-ranges":
                    segments += 1
    out(f"ANE nodes: {found}")
    out(f"ANE nodes with segment-ranges: {segments}")
    return found


def find_mtd(sysfs, dev):
    """The device path of the one MTD named adt. Raises ADTError when there is not exactly one."""
    names = {}
    try:
        entries = sorted(os.listdir(sysfs))
    except OSError as e:
        raise ADTError(f"cannot list {sysfs}: {e}") from e
    for entry in entries:
        if not MTD_DIR.fullmatch(entry):
            continue
        try:
            with open(os.path.join(sysfs, entry, "name")) as f:
                names[entry] = f.read().strip()
        except OSError:
            continue
    hits = [e for e, n in names.items() if n == "adt"]
    if len(hits) != 1:
        raise ADTError(f"want exactly one MTD named adt, found {len(hits)} among {sorted(names.items())}")
    return os.path.join(dev, hits[0] + "ro"), os.path.join(sysfs, hits[0], "size")


def read_adt(path_arg):
    if path_arg:
        with open(path_arg, "rb") as f:
            return f.read(READ_MAX)
    dev_path, size_path = find_mtd(os.environ.get("ANE_ADT_SCAN_SYSFS", "/sys/class/mtd"),
                                   os.environ.get("ANE_ADT_SCAN_DEV", "/dev"))
    try:
        with open(size_path) as f:
            size = int(f.read().strip(), 0)
    except (OSError, ValueError) as e:
        raise ADTError(f"cannot read the size of the adt MTD: {e}") from e
    if not 8 <= size <= READ_MAX:
        raise ADTError(f"the adt MTD size {size} is outside 8..{READ_MAX}")
    with open(dev_path, "rb") as f:
        data = f.read(size)
    if len(data) != size:
        raise ADTError(f"short read of {dev_path}: {len(data)} of {size} bytes")
    return data


def main(argv):
    if len(argv) > 2 or (len(argv) == 2 and argv[1].startswith("-")):
        print("usage: ane_adt_scan.py [ADT.bin]", file=sys.stderr)
        return 2
    try:
        adt = read_adt(argv[1] if len(argv) == 2 else None)
        found = scan(adt, print)
    except (ADTError, OSError) as e:
        print(f"ane_adt_scan: {e}", file=sys.stderr)
        return 2
    return 0 if found else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
