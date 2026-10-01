#!/usr/bin/env python3
"""Compare ane_dart_probe reads with the ADT DART tunables.

usage: dart_compare.py DART_TUNABLES_TSV LOG [LOG...]

DART_TUNABLES_TSV is receipts/2026-10-01-t6021-macos-vs-linux-mmio/dart-tunables.tsv.
Each LOG holds "ane_dart_probe: 0x<PA> = 0x<value>" lines (dmesg or netconsole).
Prints one row per ADT word: instance, offset, mask, value, the read of each log,
and applied = (read & mask) == value. Then every word that differs between logs."""
import re
import sys
from pathlib import Path

LINE = re.compile(r"ane_dart_probe: (0x[0-9a-f]+) = (0x[0-9a-f]+)")


def reads(path):
    return {int(a, 16): int(v, 16) for a, v in LINE.findall(Path(path).read_text())}


def main():
    tsv, logs = sys.argv[1], sys.argv[2:]
    runs = [reads(p) for p in logs]
    names = [Path(p).stem for p in logs]
    print("instance\toffset\tmask\tvalue\t" + "\t".join(f"read {n}\tapplied {n}" for n in names))
    tally = {}
    for row in Path(tsv).read_text().splitlines()[1:]:
        inst, pa, off, mask, value = row.split("\t")[:5]
        pa, mask, value = int(pa, 16), int(mask, 16), int(value, 16)
        cells = []
        for n, r in zip(names, runs):
            if pa not in r:
                cells += ["not read", "-"]
                continue
            ok = (r[pa] & mask) == value
            tally.setdefault((inst, n), [0, 0])[0 if ok else 1] += 1
            cells += [f"{r[pa]:#010x}", "yes" if ok else "NO"]
        print(f"{inst}\t{off}\t{mask:#010x}\t{value:#010x}\t" + "\t".join(cells))
    for (inst, n), (yes, no) in sorted(tally.items()):
        print(f"# {inst} {n}: applied {yes}, not applied {no}")
    for i in range(1, len(runs)):
        diff = sorted(a for a in runs[0].keys() | runs[i].keys() if runs[0].get(a) != runs[i].get(a))
        print(f"# {names[0]} vs {names[i]}: {len(runs[i])} words, {len(diff)} differ" +
              "".join(f"\n#   {a:#x}: {runs[0].get(a, 0):#010x} -> {runs[i].get(a, 0):#010x}" for a in diff))


if __name__ == "__main__":
    main()
