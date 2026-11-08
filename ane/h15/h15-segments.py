#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Print the fw_seg= argument for ane_h15 stage=5 map_fw=1 from an ADT dump.

Usage: h15-segments.py ADT.bin [RVBAR]

RVBAR is the IORVBAR a stage 2 run read on this SoC (default 0x10000000001, the T6030 value of PR 175). ane_h15
maps TEXT at RVBAR & ~1, so the script refuses a dump whose TEXT remap is another address.

ADT.bin must be the live ADT of the SAME boot you will insmod on: iBoot picks the physical addresses when it
loads the firmware. Whether those addresses repeat from one boot to the next is NOT KNOWN, so do not reuse a
dump from another boot. With m1n1's proxyclient attached to that boot, the dump is
    python3 -c "from m1n1.setup import *; open('adt.bin', 'wb').write(u.get_adt())"
(proxyutils.get_adt reads devtree at boot_args.devtree). Never from macOS.

The ANE node's segment-ranges holds two 32-byte little-endian records {phys, iova offset, remap iova,
size | flags << 32}. The node is /arm-io/ane0 in the ADT of a J516C and /arm-io/ane in the M6 ADT the script was first
checked against; the lookup tries ane0 first. This script walks the ADT without m1n1's python modules, prints the records and the
fw_seg= argument, and refuses anything that is not exactly two records, a layout where remap != vm-base + offset,
or a DATA offset that is not the TEXT size (ane_h15 derives both IOVAs from the RVBAR entry and would be wrong).
Checked against the saved M6 ADT: it prints fw_seg=100014e8000:c4000,100030c4000:2ac000, from that ADT. Nothing
is written.
"""
import struct
import sys


def parse(b, off=0):
    nprops, nchild = struct.unpack_from('<II', b, off)
    off += 8
    props = {}
    for _ in range(nprops):
        name = b[off:off + 32].split(b'\0')[0].decode('latin1')
        size = struct.unpack_from('<I', b, off + 32)[0] & 0x7fffffff
        off += 36
        props[name] = b[off:off + size]
        off += (size + 3) & ~3
    kids = []
    for _ in range(nchild):
        kid, off = parse(b, off)
        kids.append(kid)
    return {'props': props, 'kids': kids}, off


def find(node, *path):
    for want in path:
        for k in node['kids']:
            if k['props'].get('name', b'').split(b'\0')[0].decode() == want:
                node = k
                break
        else:
            return None
    return node


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    rvbar = int(sys.argv[2], 0) if len(sys.argv) == 3 else 0x10000000001
    with open(sys.argv[1], 'rb') as f:
        data = f.read()
    try:
        root, _ = parse(data)
    except (struct.error, IndexError, UnicodeDecodeError, RecursionError, MemoryError):
        sys.exit('this file is not an ADT dump')
    ane = find(root, 'arm-io', 'ane0') or find(root, 'arm-io', 'ane')
    if ane is None:
        sys.exit('no /arm-io/ane node in this ADT')
    seg = ane['props'].get('segment-ranges')
    if seg is None or len(seg) != 64:
        sys.exit(f'segment-ranges is {"absent" if seg is None else str(len(seg)) + " bytes"}, expected exactly two 32-byte records')
    recs = [struct.unpack_from('<QQQQ', seg, 32 * i) for i in range(2)]
    for i, (phys, off, remap, sz) in enumerate(recs):
        print(f'record {i}: phys={phys:#x} iova_offset={off:#x} remap={remap:#x} size={sz & 0xffffffff:#x} flags={sz >> 32:#x}')
    text, data_ = recs
    if text[2] != rvbar & ~1:
        sys.exit(f'TEXT remap {text[2]:#x} is not the RVBAR entry {rvbar & ~1:#x}: ane_h15 would map TEXT at the wrong address')
    vm_base = text[2] - text[1]
    for _, off, remap, _ in recs:
        if remap != vm_base + off:
            sys.exit('remap != vm_base + offset: the layout differs from the M6, ane_h15 would derive wrong IOVAs')
    if data_[1] != text[3] & 0xffffffff:
        sys.exit('DATA offset is not the TEXT size: ane_h15 puts DATA directly behind TEXT and would be wrong')
    print(f'fw_seg={text[0]:x}:{text[3] & 0xffffffff:x},{data_[0]:x}:{data_[3] & 0xffffffff:x}')


if __name__ == '__main__':
    main()
