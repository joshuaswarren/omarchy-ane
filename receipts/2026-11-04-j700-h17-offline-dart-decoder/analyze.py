#!/usr/bin/env python3
"""Offline, bounded Apple DART evidence extraction; no device access."""
import hashlib
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / '2026-11-04-j700-h17-firmware-load-path/scripts'))
import ane
import kimage
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

digest = hashlib.sha256(ane.data()).hexdigest()
assert digest == '7d4ca8baa2f40da0244eeeadebd072895995cf2046e1ef2cfc9f893c22af44fe'
g = kimage.G()
with (HERE / 'identity-symbols.txt').open('w') as out:
    print('kernelcache', ane.KC, 'sha256', digest, file=out)
    for seg in kimage._parse(0)[1]:
        print('top-level segment', seg, file=out)
        if seg[4]:
            assert seg[1] - seg[3] == ane.BASE
    for a, n, k in zip(g.addrs, g.names, g.kexts_by_sym):
        if k == 'com.apple.driver.AppleT8110DART':
            print(hex(a), n, file=out)

md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
md.detail = True
ranges = {
    'collect': (0xfffffe000978aed8, 0xfffffe000978b250),
    'fatal': (0xfffffe0009789854, 0xfffffe0009789ef4),
    'handle-error': (0xfffffe000978a3e4, 0xfffffe000978acb8),
    'interrupt': (0xfffffe000978bb0c, 0xfffffe000978c000),
    'version': (0xfffffe0009787018, 0xfffffe0009787058),
}
for name, (lo, hi) in ranges.items():
    with (HERE / (name + '.txt')).open('w') as out:
        print('sha256', digest, 'range', hex(lo), hex(hi), file=out)
        previous = None
        for ins in md.disasm(ane.raw(lo, hi-lo), lo):
            note = ''
            if ins.mnemonic in ('bl', 'b') and ins.operands[0].type == 2:
                note = str(g.near(ins.operands[0].imm % (1<<64)))
            if previous and previous.mnemonic == 'adrp' and ins.mnemonic == 'add' and ins.operands[1].reg == previous.operands[0].reg and ins.operands[2].type == 2:
                va = (previous.operands[1].imm + ins.operands[2].imm) % (1<<64)
                raw = ane.raw(va, 512).split(b'\0')[0]
                if raw and all(32 <= c < 127 or c in (9,10,13) for c in raw):
                    note += ' string@' + hex(va) + ' ' + repr(raw.decode())
            print(f'{ins.address:#x}: {ins.bytes.hex(" ")}\t{ins.mnemonic}\t{ins.op_str} {note}'.rstrip(), file=out)
            previous = ins

pin = '4fdd0fc5c931ef9f14474942b37c3d451e97fc04'
source = subprocess.check_output(['git', '-C', '/home/admin/linux-aurora', 'show', pin + ':drivers/iommu/apple-dart.c'], text=True)
lines = source.splitlines()
with (HERE / 'linux-source-excerpts.txt').open('w') as out:
    print('drivers/iommu/apple-dart.c @', pin, file=out)
    selected = set()
    for i, line in enumerate(lines):
        if any(s in line for s in ['#define DART_T8110_EXCEPTION', '#define DART_T8110_ERR_SID', 'static const char *apple_dart_t8110_v2_fault_name', 'static irqreturn_t apple_dart_t8110_v2_irq']):
            selected.update(range(max(0,i-2), min(len(lines), i + (100 if line.startswith('static') else 12))))
    for i in sorted(selected):
        print(f'{i+1}: {lines[i]}'.rstrip(), file=out)
intro = subprocess.check_output(['git', '-C', '/home/admin/linux-aurora', 'show', '--no-patch', 'f57d9004043387ed6c437ddf0afcfcc1bb49f2fd'], text=True)
(HERE / 'linux-introduction.txt').write_text('\n'.join(line.rstrip() for line in intro.splitlines()) + '\n')
print('Wrote evidence to', HERE)
