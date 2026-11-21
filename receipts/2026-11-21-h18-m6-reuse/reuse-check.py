#!/usr/bin/env python3
"""Check an hs_prog_reuse run of the 64-lane H18 add from its kernel log.
usage: reuse-check.py PROG.bin KMSG.log N
Pass needs: calls 1..N each with one 'finished in X ns' line, in order; one digest per call, all equal;
the first and last call's dumps give every lane exactly a+b in fp16; one 'sequence result 0' (one load);
'boot lifecycle clear' and RESULT PASS."""
import math, re, statistics, struct, sys
from pathlib import Path

CALL = re.compile(r'prog: call (\d+)/(\d+) finished in (\d+) ns$')
DIG = re.compile(r'prog: out buf 4 call (\d+)/(\d+) digest ([0-9a-f]{16})$')
DUMP = re.compile(r'prog: out buf 4 \+(0x[0-9a-f]+): ((?:[0-9a-f]{8} ?)+)$')
END = re.compile(r'prog: sequence result (-?\d+)$')

def expected_lanes(path):
    blob = path.read_bytes()
    _, _, sections, surfaces, _ = struct.unpack_from('<IIIIQ', blob)
    inputs = {}
    for i in range(surfaces):
        slot, _, _, offset, size = struct.unpack_from('<IIQQQ', blob, 32 * (1 + sections + i))
        if slot in (5, 6):
            inputs[slot] = blob[offset:offset + size]
    a, b = inputs[5], inputs[6]
    return [struct.unpack('<H', struct.pack('<e', struct.unpack_from('<e', a, off)[0] + struct.unpack_from('<e', b, off)[0]))[0]
            for off in range(0, len(a), 64)]

def check(lanes, lines, n):
    ns, digests, dumps, results, cur = [], {}, {}, [], 0
    clear = any('boot lifecycle clear' in l for l in lines)
    passed = any('verdict=PASS' in l and 'RESULT stage=boot' in l for l in lines)
    for line in lines:
        line = line.rstrip('\n')
        if m := CALL.search(line):
            i, tot, x = map(int, m.groups())
            if tot != n or i != len(ns) + 1 or x <= 0:
                raise ValueError(f'call line out of order or bad: {line}')
            ns.append(x); cur = i
        elif m := DIG.search(line):
            i, tot = int(m[1]), int(m[2])
            if tot != n or i != cur or i in digests:
                raise ValueError(f'digest line out of order: {line}')
            digests[i] = m[3]
        elif m := DUMP.search(line):
            d = dumps.setdefault(cur, {})
            off = int(m[1], 0)
            for j, w in enumerate(m[2].split()):
                d[off + 4 * j] = int(w, 16)
        elif m := END.search(line):
            results.append(int(m[1]))
    if len(ns) != n or len(digests) != n:
        raise ValueError(f'expected {n} calls and digests, got {len(ns)} and {len(digests)}')
    if len(set(digests.values())) != 1:
        bad = [i for i, d in digests.items() if d != digests[1]]
        raise ValueError(f'output digest differs from call 1 in calls {bad[:10]}')
    for i in sorted({1, n}):
        d = dumps.get(i, {})
        wrong = [j for j, v in enumerate(lanes) if d.get(64 * j) != v]
        if wrong:
            raise ValueError(f'call {i}: wrong or missing lanes {wrong[:10]}')
    if results != [0]:
        raise ValueError(f'expected one sequence result 0 (one program load), got {results}')
    if not clear or not passed:
        raise ValueError('module did not report boot lifecycle clear and RESULT PASS')
    return ns

def main(prog, kmsg, n):
    n = int(n)
    lanes = expected_lanes(Path(prog))
    ns = check(lanes, Path(kmsg).read_text(errors='replace').splitlines(), n)
    s = sorted(ns)
    print(f'M6-REUSE checked={n}/{n} lanes={len(lanes)} median_us={statistics.median(ns)/1000:.3f} '
          f'p99_us={s[math.ceil(n * .99) - 1]/1000:.3f} min_us={s[0]/1000:.3f} max_us={s[-1]/1000:.3f} digest_equal=1 loads=1')

if __name__ == '__main__':
    try:
        main(*sys.argv[1:])
    except ValueError as e:
        sys.exit(f'M6-REUSE FAIL: {e}')
