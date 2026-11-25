#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren
"""hwx2anep: turn a mil-hwxc HWX program into the ANEP program file the
ane/h18 loader takes (hs_prog=1, prog_fw).

The shipped packer (tools/ane-prog-pack) only builds a program file from an
ANEC, and mil-hwxc refuses `--format anec` for H18 ("H18 artifact format must
be 'hwx'"). The loader, however, reads a program file whose sections are the
HWX's own (ane/h18/ane_h18_main.c: prog_iova_text maps section 3, the task
stream; prog_text_const copies the first 0x4000 bytes of section 2, the
const; prog_iova_kern takes section 2 behind those 0x4000 bytes, the
weights). This converter writes that file straight from the HWX.

File format (ane-prog-pack's writer, read back by the driver's
ane_h18_prog_parse):

    +0x00 u32 magic 'ANEP' | u32 version 1 | u32 nsec | u32 nio
    +0x10 u64 file size | u64 reserved
    +0x20 nsec records of 0x20: u32 section id | u32 0 | u64 size |
           u64 offset (16 byte aligned) | u64 0
    then nio records of 0x20: u32 buffer id | u32 direction (0 in, 1 out,
           2 scratch) | u64 size | u64 data offset | u64 data size
    then the payloads, each 16 byte aligned.

Section ids follow the loader's expectations, not the T6021 builder's:
id 2 carries the const (first 0x4000 bytes) and, behind it, the weights;
id 3 carries the task stream. Buffers keep the manifest's ids and sizes and
are written in the order the loader maps them: inputs first, then outputs.

Usage:
    hwx2anep.py --hwx program.hwx --manifest manifest.json -o prog.bin
"""
import argparse
import json
import struct
import sys

HWX_MAGIC = 0xBEEFFACE
H18_SUBTYPE = 10
H18G_SUBTYPE = 11
SUPPORTED_SUBTYPES = (H18_SUBTYPE, H18G_SUBTYPE)
ANEP_MAGIC = 0x50454E41      # 'ANEP', ane/h18/ane_h18.h:127
ANEP_VERSION = 1
ANEP_MAX_SECS = 9
ANEP_MAX_IO = 8
ANEP_MAX_BUF = 0x4000000     # 64 MiB
CONST_BYTES = 0x4000         # the loader's const window (prog_text_const)


def cstring(raw):
    return raw.split(b"\0", 1)[0].decode("utf-8", "replace")


def parse_hwx(data):
    """Return (subtype, sections, buffer_refs) from an HWX container.

    sections: {(segment, section): {"addr", "size", "offset"}}
    buffer_refs: [(address, name)] in file order.
    """
    if len(data) < 32:
        raise ValueError("HWX shorter than its header")
    magic, _, subtype, _, command_count, command_bytes, _, _ = \
        struct.unpack_from("<8I", data, 0)
    if magic != HWX_MAGIC:
        raise ValueError("invalid HWX magic 0x%08x" % magic)
    if 32 + command_bytes > len(data):
        raise ValueError("the load command table does not fit in the file")
    sections = {}
    refs = []
    cursor = 32
    for _ in range(command_count):
        command, command_size = struct.unpack_from("<2I", data, cursor)
        if command_size < 8 or cursor + command_size > 32 + command_bytes:
            raise ValueError("load command at 0x%x is out of range" % cursor)
        if command == 0x19:            # LC_SEGMENT_64
            if command_size < 72:
                raise ValueError("segment command at 0x%x is too short" % cursor)
            fields = struct.unpack_from("<2I16s4Q4I", data, cursor)
            segment = cstring(fields[2])
            filesize = fields[6]
            section_count = fields[-2]
            if section_count * 80 > command_size - 72:
                raise ValueError("segment at 0x%x declares sections outside "
                                 "the command" % cursor)
            sc = cursor + 72
            for index in range(section_count):
                sf = struct.unpack_from("<16s16s2Q8I", data, sc)
                name = cstring(sf[0])
                addr, size, offset = sf[2], sf[3], sf[4]
                if filesize and (offset + size > len(data)):
                    raise ValueError("section %s/%s runs past the file"
                                     % (segment, name))
                # A segment can hold several sections with the same name
                # (the HWX has two __FVMLIB/__const surfaces), so keep them
                # all.
                sections.setdefault((segment, name), []).append(
                    {"addr": addr, "size": size, "offset": offset})
                sc += 80
        elif command == 0x40 and command_size == 0x20:
            address = struct.unpack_from("<Q", data, cursor + 0x10)[0]
            refs.append((address, cstring(data[cursor + 0x18:cursor + 0x20])))
        cursor += command_size
    return subtype, sections, refs


def one(sections, key):
    """The single section with that (segment, name), or None."""
    found = sections.get(key, [])
    return found[0] if len(found) == 1 else None


def match_buffers(refs, inputs, outputs, sections):
    """Match the HWX's buffer references to the manifest's entries.

    The references carry an address and a name ('input0', 'output0'); the
    manifest carries the buffer id, the MIL name and the allocation size.
    Neither list is trusted on its own: every reference must find the
    manifest entry of the same kind (by name when the names agree, by kind
    otherwise), the HWX's buffer surface at that address must have the
    manifest's allocation size, and the addresses must form the ladder the
    loader maps them on - consecutive, 16 KiB aligned, each step the
    aligned size of the previous buffer.
    """
    if len(refs) != len(inputs) + len(outputs):
        raise ValueError("the HWX names %d buffers, the manifest %d inputs "
                         "and %d outputs" % (len(refs), len(inputs),
                                             len(outputs)))
    surfaces = {}
    for (segment, name), infos in sections.items():
        if segment == "__FVMLIB":
            for info in infos:
                surfaces[info["addr"]] = info["size"]

    def kind_of(ref_name):
        lowered = ref_name.lower()
        if lowered.startswith("input"):
            return 0
        if lowered.startswith("output"):
            return 1
        raise ValueError("buffer reference %r names no direction" % ref_name)

    entries = [(entry, 0) for entry in inputs] + [(entry, 1) for entry in outputs]
    by_name = {}
    for entry, kind in entries:
        by_name.setdefault(entry.get("name", ""), []).append((entry, kind))

    ios = []
    used = set()
    for position, (address, ref_name) in enumerate(refs):
        kind = kind_of(ref_name)
        candidates = [item for item in entries
                      if item[1] == kind and id(item[0]) not in used]
        if not candidates:
            raise ValueError("buffer reference %r has no manifest entry"
                             % ref_name)
        # Prefer an exact name match; otherwise take the first free entry of
        # the same kind and let the address and size checks confirm it.
        match = None
        for entry, entry_kind in candidates:
            if entry.get("name") == ref_name:
                match = (entry, entry_kind)
                break
        if match is None:
            match = candidates[0]
        entry, _ = match
        used.add(id(entry))
        size = entry["allocationBytes"]
        surface = surfaces.get(address)
        if surface is None:
            raise ValueError("buffer reference %r at 0x%x has no __FVMLIB "
                             "surface" % (ref_name, address))
        if surface != size:
            raise ValueError("buffer reference %r at 0x%x covers %d bytes, "
                             "the manifest says %d" % (ref_name, address,
                                                       surface, size))
        if position:
            previous = ios[-1]
            step = (previous["size"] + 0x3FFF) & ~0x3FFF
            if address != previous["addr"] + step:
                raise ValueError("buffer reference %r at 0x%x does not follow "
                                 "0x%x by its aligned size 0x%x"
                                 % (ref_name, address, previous["addr"], step))
            if address % 0x4000:
                raise ValueError("buffer reference %r at 0x%x is not 16 KiB "
                                 "aligned" % (ref_name, address))
        ios.append({"id": entry["index"], "dir": kind, "size": size,
                    "addr": address, "name": ref_name})
    return ios


def build(hwx, manifest):
    """Build the ANEP bytes from an HWX image and its compile manifest."""
    subtype, sections, refs = parse_hwx(hwx)
    if subtype not in SUPPORTED_SUBTYPES:
        raise ValueError("HWX subtype 0x%04x is not H18/H18G (0x%04x, 0x%04x)"
                         % (subtype, H18_SUBTYPE, H18G_SUBTYPE))
    text = one(sections, ("__TEXT", "__text"))
    const = one(sections, ("__TEXT", "__const"))
    if text is None:
        raise ValueError("the HWX has no __TEXT/__text section")
    if const is None:
        raise ValueError("the HWX has no __TEXT/__const section")
    if const["size"] < CONST_BYTES:
        raise ValueError("__TEXT/__const is %d bytes, the loader's const "
                         "window is %d" % (const["size"], CONST_BYTES))
    # Section 2: the const window first, the weights (if any) behind it.
    sec2 = hwx[const["offset"]:const["offset"] + CONST_BYTES]
    kern = one(sections, ("__KERN_0", "__kern_0")) or \
        one(sections, ("__FVMLIB", "__KERN_0")) or \
        one(sections, ("__KERN", "__KERN_0"))
    if kern is not None:
        sec2 += hwx[kern["offset"]:kern["offset"] + kern["size"]]
    # Section 3: the task stream.
    sec3 = hwx[text["offset"]:text["offset"] + text["size"]]

    inputs = sorted(manifest["inputs"], key=lambda i: i["index"])
    outputs = sorted(manifest["outputs"], key=lambda o: o["index"])
    ios = match_buffers(refs, inputs, outputs, sections)

    nsec, nio = 2, len(ios)
    if nsec > ANEP_MAX_SECS or nio > ANEP_MAX_IO:
        raise ValueError("too many sections or buffers")
    for entry in ios:
        if entry["size"] > ANEP_MAX_BUF:
            raise ValueError("buffer %d is larger than the loader allows"
                             % entry["id"])
    pos = 0x20 + (nsec + nio) * 0x20
    payload = []
    for blob in (sec2, sec3):
        payload.append((len(blob), blob))
    total = pos
    for size, _ in payload:
        total += (size + 15) & ~15
    total = (total + 15) & ~15
    out = bytearray(total)
    struct.pack_into("<IIII", out, 0, ANEP_MAGIC, ANEP_VERSION, nsec, nio)
    struct.pack_into("<Q", out, 16, total)
    r = 0x20
    offsets = []
    cur = pos
    for index, (size, blob) in enumerate(payload):
        sid = 2 + index
        struct.pack_into("<I", out, r, sid)
        struct.pack_into("<Q", out, r + 8, size)
        struct.pack_into("<Q", out, r + 16, cur)
        out[cur:cur + size] = blob
        cur += (size + 15) & ~15
        offsets.append(cur)
        r += 0x20
    for entry in ios:
        struct.pack_into("<I", out, r, entry["id"])
        struct.pack_into("<I", out, r + 4, entry["dir"])
        struct.pack_into("<Q", out, r + 8, entry["size"])
        struct.pack_into("<Q", out, r + 16, 0)     # data offset (unused)
        struct.pack_into("<Q", out, r + 24, 0)     # data size: zero filled
        r += 0x20
    return bytes(out), {
        "subtype": subtype,
        "sections": [{"id": 2, "size": len(sec2)}, {"id": 3, "size": len(sec3)}],
        "buffers": ios,
        "text_addr": text["addr"],
        "const_addr": const["addr"],
        "const_copy_offset": const["addr"] - text["addr"],
        "kern_addr": kern["addr"] if kern is not None else None,
        "kern_slot": 1 if kern is not None else 0,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--hwx", required=True)
    ap.add_argument("--manifest", required=True)
    ap.add_argument("-o", "--output",
                    help="the program file to write (not needed with "
                         "--print-plan)")
    ap.add_argument("--print-plan", action="store_true",
                    help="print the loader's fixed-IOVA plan and write nothing")
    args = ap.parse_args()
    if not args.print_plan and not args.output:
        ap.error("an output path is needed unless --print-plan is given")
    blob, plan = build(open(args.hwx, "rb").read(),
                        json.load(open(args.manifest)))
    if args.print_plan:
        print("text IOVA      0x%x" % plan["text_addr"])
        print("const IOVA     0x%x (copy at +0x%x in the text region)"
              % (plan["const_addr"], plan["const_copy_offset"]))
        print("const copy     %d bytes" % CONST_BYTES)
        if plan["kern_addr"] is not None:
            print("kern IOVA      0x%x (prog_kern_slot=%d)"
                  % (plan["kern_addr"], plan["kern_slot"]))
        else:
            print("kern           absent (no weights)")
        for entry in plan["buffers"]:
            print("buffer id=%d dir=%d size=%d addr=0x%x name=%s"
                  % (entry["id"], entry["dir"], entry["size"], entry["addr"],
                     entry["name"]))
        return 0
    with open(args.output, "wb") as handle:
        handle.write(blob)
    print("wrote %s: %d bytes, %d sections, %d buffers"
          % (args.output, len(blob), len(plan["sections"]),
             len(plan["buffers"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
