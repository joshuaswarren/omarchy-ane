#!/usr/bin/env python3
"""Verify independent Apple instruction anchors for the Linux fault decoder."""
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent.parent /
                       '2026-11-04-j700-h17-firmware-load-path/scripts'))
import ane
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

SHA = '7d4ca8baa2f40da0244eeeadebd072895995cf2046e1ef2cfc9f893c22af44fe'
if hashlib.sha256(ane.data()).hexdigest() != SHA:
    raise SystemExit('kernelcache digest mismatch')
md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
anchors = [
    (0xfffffe000978af34, 'lsl', 'w8, w23, #6'),
    (0xfffffe000978af38, 'add', 'w8, w8, #8, lsl #12'),
    (0xfffffe000978af40, 'ldr', 'w21, [x9, w8, uxtw]'),
    (0xfffffe000978af4c, 'str', 'w21, [x19, #0x10]'),
    (0xfffffe000978af68, 'mov', 'w9, #0x800c'),
    (0xfffffe000978af6c, 'add', 'w9, w9, w23, lsl #6'),
    (0xfffffe000978afa8, 'ldr', 'w8, [x10]'),
    (0xfffffe000978afac, 'lsl', 'x24, x8, #0x20'),
    (0xfffffe000978afcc, 'mov', 'w9, #0x8008'),
    (0xfffffe000978afd0, 'add', 'w9, w9, w23, lsl #6'),
    (0xfffffe000978b00c, 'ldr', 'w8, [x10]'),
    (0xfffffe000978b018, 'orr', 'x8, x8, x24'),
    (0xfffffe000978b01c, 'str', 'x8, [x19, #8]'),
    (0xfffffe00097899f4, 'and', 'w8, w22, #0xf'),
    (0xfffffe0009789a18, 'ubfx', 'w9, w22, #0xc, #2'),
    (0xfffffe0009789a50, 'cmp', 'w9, #1'),
    (0xfffffe0009789a6c, 'add', 'x11, x11, #0x9a8'),
]
for va, mnemonic, operands in anchors:
    ins = next(md.disasm(ane.raw(va, 4), va))
    if (ins.mnemonic, ins.op_str) != (mnemonic, operands):
        raise SystemExit(f'instruction mismatch at {va:#x}')
if ane.raw(0xfffffe00076199a8, 12) != b'CTE invalid\0':
    raise SystemExit('CTE label mismatch')
result = dict(kernelcache_sha256=SHA, checked_anchors=len(anchors),
              independent_address_layout='0x8000 + (sid << 6); low +8, high +12',
              address_assembly='u32(low) | (u64(u32(high)) << 32)',
              status_0x1100=dict(code=0, level=1, label='CTE invalid',
                                 access_bits=hex(0x1100 & 0x700)),
              hardware_qualification=False)
Path(__file__).with_name('decoder-verification.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result, indent=2))
