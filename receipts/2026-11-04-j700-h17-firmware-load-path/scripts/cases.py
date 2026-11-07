#!/usr/bin/env python3
"""Cross-family case table for ANEHWDevice::initializeANESoCConfig.

Simulates the `cmp w8,#K` dispatch tree (rooted at the first
`ldr w8,[x19,#0x1cc]`) for every candidate platform key, then extracts
the per-case field stores: family id ([x21]), variant (this+0x1c8),
q-block (stur q0,[x23,#0x44], x23 = this+0x104), d-pair
(str d0,[x19,#0x158]), and CPU-control offset (this+0x160).

Usage: python3 cases.py [dump-file]   (default h17-socconfig-all.txt)
"""
import re
import struct
import sys

import ane

DUMP = sys.argv[1] if len(sys.argv) > 1 else '../analysis/h17-socconfig-all.txt'
PAT = re.compile(r'^(0x[0-9a-f]+): ([0-9a-f ]+)\t([a-z][a-z0-9.]*)\t(.*)$')
POOL = 0xfffffe00073b0000

ins = []
with open(DUMP) as f:
    for line in f:
        m = PAT.match(line)
        if m:
            ins.append(m.groups())
idx = {int(va, 16): i for i, (va, _, _, _) in enumerate(ins)}


def imm(s):
    return int(s.strip().lstrip('#'), 0)


is_anchor = [False] * len(ins)
for i, (_, _, mnem, ops) in enumerate(ins):
    if mnem == 'str' and re.match(r'w\d+, \[x21\]$', ops):
        if any(m2 == 'str' and re.match(r'w\d+, \[x19, #0x1c8\]', o2)
               for _, _, m2, o2 in ins[i + 1:i + 12]):
            is_anchor[i] = True
anchors = [i for i, a in enumerate(is_anchor) if a]
ERR = idx.get(0xfffffe0008f66a38)
START = next(i for i, (_, _, mnem, ops) in enumerate(ins)
             if mnem == 'ldr' and re.match(r'w8, \[x19, #0x1cc\]', ops))

CONDS = ('b.eq', 'b.ne', 'b.gt', 'b.le', 'b.hi', 'b.ls', 'b.lt', 'b.ge')


def sim(key):
    """Walk the dispatch tree with w8 = key; return the case anchor or None."""
    i, w8, cmpv, steps = START, key, None, 0
    while 0 <= i < len(ins) and steps < 3000:
        steps += 1
        if is_anchor[i]:
            return i
        if i == ERR:
            return None
        _, _, mnem, ops = ins[i]
        if mnem == 'ldr' and re.match(r'w8, \[x19, #0x1cc\]', ops):
            i += 1
        elif mnem == 'cmp':
            mm = re.match(r'w8, (#0x[0-9a-f]+|#\d+)$', ops)
            if mm:
                cmpv = imm(mm.group(1))
            i += 1
        elif mnem in CONDS:
            if cmpv is None:
                return None
            c = mnem[2:]
            taken = {'eq': w8 == cmpv, 'ne': w8 != cmpv, 'gt': w8 > cmpv,
                     'le': w8 <= cmpv, 'hi': w8 > cmpv, 'ls': w8 <= cmpv,
                     'lt': w8 < cmpv, 'ge': w8 >= cmpv}[c]
            if taken:
                j = idx.get(int(ops.lstrip('#'), 16))
                if j is None:
                    return None
                i = j
            else:
                i += 1
        elif mnem == 'b':
            j = idx.get(int(ops.lstrip('#'), 16))
            if j is None:
                return None
            i = j
        else:
            i += 1
    return None


def cstring(va):
    return ane.data()[ane.off(va):ane.off(va) + 64].split(b'\0')[0].decode(
        'ascii', 'replace')


def val_of(reg, s, e):
    v = None
    for i in range(s, e):
        _, _, mnem, ops = ins[i]
        mm = re.match(r'(w\d+|x\d+), (#0x[0-9a-f]+|#\d+)$', ops)
        if mnem == 'mov' and mm and mm.group(1) == reg:
            v = imm(mm.group(2))
        elif mnem == 'movk':
            mm = re.match(r'(w\d+|x\d+), (#0x[0-9a-f]+|#\d+), lsl #(\d+)', ops)
            if mm and mm.group(1) == reg and v is not None:
                sh = int(mm.group(3))
                v = (v & ~(0xffff << sh)) | (imm(mm.group(2)) << sh)
        elif mnem == 'orr':
            mm = re.match(r'(w\d+), \1, (#0x[0-9a-f]+|#\d+)$', ops)
            if mm and mm.group(1) == reg and v is not None:
                v |= imm(mm.group(2))
        elif mnem == 'add':
            mm = re.match(r'(x\d+), \1, (#0x[0-9a-f]+|#\d+)(?:, lsl #(\d+))?$', ops)
            if mm and mm.group(1) == reg and v is not None:
                v += imm(mm.group(2)) << (int(mm.group(3)) if mm.group(3) else 0)
        elif mnem == 'sub':
            mm = re.match(r'(x\d+|w\d+), \1, (#0x[0-9a-f]+|#\d+)$', ops)
            if mm and mm.group(1) == reg and v is not None:
                v -= imm(mm.group(2))
    return v


for n, fi in enumerate(anchors):
    end = anchors[n + 1] if n + 1 < len(anchors) else len(ins)
    keys = [k for k in range(0, 0x420) if sim(k) == fi]
    # a case stores the key directly unless the single instruction before the
    # str is its own `mov w8,#K` (a mov/movk pair immediately ahead of the str)
    fam = val_of('w8', fi - 2, fi) if re.match(r'mov\tw8, #', '\t'.join(ins[fi - 1][2:4])) else None
    sva = None
    adrp = lastadd = None
    qsrc = dsrc = rawq = rawd = None
    q = d = cfg = var = None
    for i in range(fi, end):
        _, _, mnem, ops = ins[i]
        if mnem == 'adrp':
            mm = re.match(r'x8, #(0x[0-9a-f]+)$', ops)
            if mm:
                adrp = int(mm.group(1), 16)
        elif mnem == 'add':
            mm = re.match(r'x8, x8, #?(0x[0-9a-f]+|\d+)$', ops)
            if mm and adrp is not None:
                lastadd = adrp + int(mm.group(1), 0)
        elif mnem == 'str' and re.match(r'x\d+, \[x21, #0x10\]$', ops):
            sva = lastadd
        elif mnem == 'ldr':
            mm = re.match(r'q\d+, \[x8, #(0x[0-9a-f]+)\]', ops)
            if mm:
                if adrp == POOL:
                    qsrc = int(mm.group(1), 16)
                else:
                    rawq = '%#x+%s' % (adrp, mm.group(1))
            mm = re.match(r'd\d+, \[x8, #(0x[0-9a-f]+)\]', ops)
            if mm:
                if adrp == POOL:
                    dsrc = int(mm.group(1), 16)
                else:
                    rawd = '%#x+%s' % (adrp, mm.group(1))
        elif mnem == 'stur' and '[x23, #0x44]' in ops:
            q = qsrc
            qsrc = None
        elif mnem == 'str':
            mm = re.match(r'd\d+, \[x19, #0x158\]', ops)
            if mm:
                d = dsrc
                dsrc = None
                continue
            mm = re.match(r'(w\d+|x\d+), \[x19, #(0x[0-9a-f]+)\]', ops)
            if mm:
                off = int(mm.group(2), 16)
                if off == 0x160 and cfg is None:
                    cfg = val_of(mm.group(1), fi, i)
                elif off == 0x1c8 and var is None:
                    var = val_of(mm.group(1), fi, i)
    name = cstring(sva) if sva else '?'
    qs = 'runtime' if q is None and i >= fi else ('pool%+#x' % q if q is not None else 'runtime')
    print('%s  keys=%s  name=%r  fam=%s  var=%s  q=%s  d=%s  cfg=%s' % (
        ins[fi][0],
        ','.join(hex(k) for k in keys) if keys else '?',
        name,
        hex(fam) if fam is not None else 'key',
        hex(var) if var is not None else '?',
        ('pool%+#x' % q) if q is not None else ('runtime'),
        ('pool%+#x' % d) if d is not None else 'runtime',
        hex(cfg) if cfg is not None else 'runtime'))
    if q is not None:
        vals = struct.unpack('<4I', ane.data()[ane.off(POOL + q):ane.off(POOL + q) + 16])
        print('      +0x148..0x154 = %s' % ' '.join('%#x' % v for v in vals))
    if d is not None:
        v = ane.u64(POOL + d)
        print('      +0x158/0x15c  = %#x / %#x' % (v & 0xffffffff, v >> 32))
