#!/usr/bin/env python3
"""OwnMemGate: compare the ane_t6021 probe trace of two boots. Takes the ane_t6021 kernel lines
(postboot.sh dmesg-ane.txt), drops the timestamp and masks hex numbers and decimals, and prints a
unified diff of the two sequences. Masking keeps the line text and order, so a missing, added or
reordered BOOT-PHASE line shows; a changed value does not.

usage: trace_cmp.py OLD_DMESG_ANE NEW_DMESG_ANE"""
import difflib
import re
import sys


def norm(path):
    out = []
    for line in open(path, errors="replace"):
        if "ane_t6021" not in line:
            continue
        line = re.sub(r"^\[\s*[\d.]+\]\s*", "", line.rstrip("\n"))
        line = re.sub(r"0x[0-9a-fA-F]+|\b[0-9a-f]{6,}\b", "H", line)
        out.append(re.sub(r"\b\d+(\.\d+)?\b", "N", line))
    return out


a, b = norm(sys.argv[1]), norm(sys.argv[2])
diff = list(difflib.unified_diff(a, b, sys.argv[1], sys.argv[2], n=0, lineterm=""))
print(f"lines: {len(a)} vs {len(b)}; BOOT-PHASE {sum('BOOT-PHASE' in x for x in a)} vs {sum('BOOT-PHASE' in x for x in b)}")
print("\n".join(diff) if diff else "IDENTICAL after masking")
