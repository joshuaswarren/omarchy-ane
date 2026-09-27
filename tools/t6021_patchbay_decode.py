#!/usr/bin/env python3
"""Decode the iBoot patchbay deltas between the archived 13.5 selene image and
the actually-preloaded DATA segment captured before Linux.

Offsets in comparison.json data-segment ranges are relative to the DATA
segment start (VM 0xc4000). Each patched field is annotated with:
  - segment offset and firmware VM address
  - archive bytes vs live (preloaded) bytes
  - printable ASCII context around the field (the patchbay string tags such
    as "SoC_", "SoCR", "CpAd", "WrAd" sit adjacent to the fields they name)

Usage:
  python3 t6021_patchbay_decode.py <archive.macho> <data-segment.bin> \
      [comparison.json]

Exit 0 on success. Read-only; no device access.
"""
import json
import pathlib
import struct
import sys

DATA_VM_BASE = 0xC4000
DATA_FILE_BASE = 0xC8000
CTX = 24


def symtab(blob: bytes):
    """Parse LC_SYMTAB from the macho: returns sorted [(value, name)]."""
    ncmds = struct.unpack_from('<I', blob, 16)[0]
    off = 32
    syms = []
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from('<II', blob, off)
        if cmd == 0x2:  # LC_SYMTAB
            symoff, nsyms, stroff, strsize = struct.unpack_from('<IIII', blob, off + 8)
            strings = blob[stroff:stroff + strsize]
            for i in range(nsyms):
                s = symoff + i * 16
                name_off = struct.unpack_from('<I', blob, s)[0]
                value = struct.unpack_from('<Q', blob, s + 8)[0]
                end = strings.find(b'\0', name_off)
                name = strings[name_off:end].decode(errors='replace')
                if name:
                    syms.append((value, name))
            break
        off += cmdsize
    return sorted(syms)


def sym_for(syms, vm):
    """Greatest symbol at or before vm (the __rtk_patch_* tag naming it)."""
    best = None
    for value, name in syms:
        if value <= vm:
            best = (value, name)
        else:
            break
    return best


def ascii_ctx(buf: bytes, start: int, length: int) -> str:
    lo = max(0, start - CTX)
    hi = min(len(buf), start + length + CTX)
    out = []
    for b in buf[lo:hi]:
        out.append(chr(b) if 0x20 <= b < 0x7F else '.')
    mark = ''
    for i in range(lo, hi):
        if start <= i < start + length:
            mark += '^'
        else:
            mark += ' '
    return ''.join(out) + '\n           ' + mark


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    archive = pathlib.Path(sys.argv[1]).read_bytes()
    live = pathlib.Path(sys.argv[2]).read_bytes()
    cmp_path = pathlib.Path(sys.argv[3]) if len(sys.argv) > 3 else None

    if cmp_path:
        ranges = [(int(r['offset'], 0), r['length'],
                   bytes.fromhex(r['archive']), bytes.fromhex(r['live']))
                  for r in json.loads(cmp_path.read_text())
                  ['data-segment']['ranges']]
    else:
        if len(live) != len(archive[DATA_FILE_BASE:DATA_FILE_BASE + len(live)]):
            print('live DATA length differs from archive slice; pass comparison.json')
            return 2
        ranges = []
        base = archive[DATA_FILE_BASE:DATA_FILE_BASE + len(live)]
        i = 0
        while i < len(live):
            if base[i] != live[i]:
                j = i
                while j < len(live) and base[j] != live[j]:
                    j += 1
                ranges.append((i, j - i, base[i:j], live[i:j]))
                i = j
            else:
                i += 1

    print(f'{len(ranges)} patched ranges in DATA segment '
          f'(segment-relative; VM = offset + {DATA_VM_BASE:#x}):')
    syms = symtab(archive)
    for off, length, arch, lv in sorted(ranges):
        vm = DATA_VM_BASE + off
        fo = off + DATA_FILE_BASE
        sym = sym_for(syms, vm)
        symtxt = ''
        if sym:
            sva, sname = sym
            symtxt = (f'  sym {sname} @vm {sva:#x} (+{vm - sva:#x})')
        # bytes as bytes; candidate little-endian decodes labeled as candidates
        cand = ''
        if length <= 8:
            pad = arch + b'\0' * (8 - len(arch))
            lp = lv + b'\0' * (8 - len(lv))
            cand = (f'  cand-le32 arch {struct.unpack("<I", pad[:4])[0]:#x} '
                    f'-> live {struct.unpack("<I", lp[:4])[0]:#x}')
        print(f'\n  seg 0x{off:06x}  vm 0x{vm:06x}  file 0x{fo:06x}  len {length}: '
              f'archive[{arch.hex()}] -> live[{lv.hex()}] (bytes){cand}{symtxt}')
        print('           ' + ascii_ctx(live, off, length))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
