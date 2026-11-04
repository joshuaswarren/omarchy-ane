#!/usr/bin/env python3
"""Cross-check the decoded (offset, width, mask, value) against each element's own
mangled symbol name `_fig_<offset>_<widthbits>_<mask>_<value>_<callback>`.

This is the check that validates the width/offset split independently of how
word0 is read.  Expected: 62 distinct elements / 67 table rows, 0 mismatches.
"""
import json, re, importlib.util, os

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("v", os.path.join(HERE, "verify_final.py"))
v = importlib.util.module_from_spec(spec); spec.loader.exec_module(v)

syms = {}
for e in json.load(open("/var/tmp/neo/syms_HAL.json")):
    syms.setdefault(e["value"], e["name"])

TABS = [0xfffffe000813baf0, 0xfffffe000813bc10, 0xfffffe000813bc28,
        0xfffffe000813bc58, 0xfffffe000813be50, 0xfffffe000813be98,
        0xfffffe000813be30, 0xfffffe000813be40, 0xfffffe000813be20,
        0xfffffe000813be10]

rows = [r for t in TABS for r in v.walk(t)[0]]
uniq = {r["elem"]: r for r in rows}
pat = re.compile(r"_fig_([0-9a-f]+)_(\d+)_([0-9a-f]+)_([0-9a-f]+)_(null|\w+)$")
bad = checked = 0
for elem, r in uniq.items():
    m = pat.search(syms[elem])
    checked += 1
    if not (int(m.group(1), 16) == r["off"] and int(m.group(2)) == r["width"] * 8
            and int(m.group(3), 16) == r["mask"] and int(m.group(4), 16) == r["value"]):
        bad += 1
        print("MISMATCH", hex(elem), syms[elem], r["off"], r["width"] * 8,
              hex(r["mask"]), hex(r["value"]))
print("table rows=%d distinct elements=%d checked=%d mismatches=%d"
      % (len(rows), len(uniq), checked, bad))
raise SystemExit(1 if bad else 0)
