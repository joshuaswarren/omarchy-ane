#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren
"""Generate the window-3 D script from the live box's own `dtrace -l -P fbt` listing.

  w3gen.py FBT_LISTING --out w3.d --probes probes.json [--no-proc] [--no-stack] [--hot-cap N]

Every clause names a probe that exists in the listing (a missing name stops dtrace). Targets are
matched by regex on the mangled function name, `.cold.N` split-offs excluded. A target whose return
probe is absent gets its entry clause only and is listed under "absent"; its thread gate is then not
used (an entry-only gate would never clear).

Output lines (quiet mode):
  E w=<ns> f=<fn> a0=.. a5=..     entry of an ANE-path function; kernel stack frames follow, indented
  R w=<ns> f=<fn> rv=0x..          return value (arg1) of an ANE-path function
  W w=<ns> f=<fn> d=<gate depth> a1=.. a2=.. a3=.. a4=..   one register write (ANEClkGen or writeReg32)
  B w=<ns> f=<fn>                  aneWorkBegin / aneWorkEnd anchors
  SEG w=<ns> seg=<n> pid=<p>       the runner exec (proc provider), numbers the ramps
  A f=<fn> seg=<n> a1=.. a2=.. a3=.. n=<count> first=<ns> last=<ns>   END aggregation of every write
`w` is walltimestamp (ns since the epoch, the clock w2sample.py stamps events with).
Hot writes (writeReg32) print per event only inside an ANE-path gate on the same thread or while the
global hot counter is under the cap; they are always aggregated.
"""
import argparse
import json
import re
import sys

# tag, module regex, function regex (mangled), args to print (this + params, max 6), kinds, role
# roles: gate = sets self->d on entry and clears on return (needs both probes); write = per-event +
# aggregate; anchor = begin/end markers. nargs for return clauses is ignored.
TARGETS = [
    ("CLPC", r"CLPC", r"clpc3ane10DVFManager\d+requestPerformanceChange", 2, "er", "gate"),
    ("CLPC", r"CLPC", r"clpc3ane10DVFManager6enable", 2, "er", "gate"),
    ("CLPC", r"CLPC", r"clpc3ane11JobTracking\d+getActiveJobRequestedOperatingState", 3, "er", "gate"),
    ("CLPC", r"CLPC", r"PMCVoterInterface\d+setANEPerfStateFloor", 3, "er", "gate"),
    ("CLPC", r"CLPC", r"PMCVoterInterface\d+writePerfStateFloorReg", 2, "er", "gate"),
    ("CLPC", r"CLPC", r"PMCVoterInterface\d+writeRegValue", 1, "er", "gate"),
    ("PMGR", r"ApplePMGR$", r"ApplePMGR\d+writeANEClkGenReg", 5, "er", "write"),
    ("PMGR", r"ApplePMGR$", r"ApplePMGR\d+readANEClkGenReg", 4, "er", "gate"),
    ("PMGR", r"ApplePMGR$", r"ApplePMGR\d+waitANEClkGenReg", 6, "er", "gate"),
    ("PMGR", r"ApplePMGR$", r"ApplePMGR\d+enableFANE", 2, "er", "gate"),
    ("PMGR", r"ApplePMGR$", r"ApplePMGR\d+_setPerfState", 4, "er", "gate"),
    ("PMGR", r"ApplePMGR$", r"ApplePMGR\d+_handlePerfStateRequest", 4, "er", "gate"),
    ("PMGR", r"ApplePMGR$", r"ApplePMGRv2\d+_handlePerfStateRequest", 4, "er", "gate"),
    ("PMGR", r"ApplePMGR$", r"ApplePMGR\d+_handleSOCPerfStateRequest", 5, "er", "gate"),
    ("PMGR", r"ApplePMGR$", r"ApplePMGR\d+_setDeviceDVFSState", 3, "er", "gate"),
    ("PMGR", r"ApplePMGR$", r"ApplePMGRv2\d+_setDeviceDVFSState", 3, "er", "gate"),
    ("PMGR", r"ApplePMGR$", r"ApplePMGR\d+_sendPMPCommand", 4, "er", "gate"),
    ("PMGR", r"ApplePMGR$", r"ApplePMGRv2\d+_sendPMPCommand", 4, "er", "gate"),
    ("PMGR", r"ApplePMGR$", r"tracePerfStateChange", 3, "er", "gate"),
    ("PMC", r"ApplePMGR$", r"ApplePMC\d+setPerfStateAgent", 6, "er", "gate"),
    ("PMC", r"ApplePMGR$", r"ApplePMC\d+writeReg32", 4, "e", "write"),
    ("PMGR", r"ApplePMGR$", r"ApplePMGR\d+writeReg32", 5, "e", "write"),
    ("SOC", r"T60\d\dPMGR", r"AppleT60\d\dPMGR\d+setPerfState", 5, "er", "gate"),
    ("SOC", r"T60\d\dPMGR", r"AppleT60\d\dPMGR\d+writeReg32", 5, "e", "write"),
    ("SOC", r"T60\d\dPMGR", r"AppleT60\d\dPMGR\d+getFANEIndex", 4, "er", "gate"),
    ("ANCHOR", r"CLPC", r"clpc4CLPC\d+aneWorkBegin", 0, "e", "anchor"),
    ("ANCHOR", r"CLPC", r"clpc4CLPC\d+aneWorkEnd", 0, "e", "anchor"),
]


def listing(path):
    """{(module, function): set(kinds)} for fbt probes, .cold.N excluded."""
    probes = {}
    for line in open(path, errors="replace"):
        f = line.split()
        if len(f) < 5 or f[1] != "fbt" or f[-1] not in ("entry", "return"):
            continue
        mod, fn = f[2], f[3]
        if ".cold." in fn or not re.fullmatch(r"[A-Za-z0-9_$]+", fn):
            continue
        probes.setdefault((mod, fn), set()).add(f[-1])
    return probes


def resolve(probes):
    """One row per target: present probes, chosen names, and what is absent."""
    rows = []
    for tag, mre, fre, nargs, kinds, role in TARGETS:
        hits = sorted((m, f) for (m, f) in probes if re.search(mre, m) and re.search(fre, f))
        row = {"tag": tag, "pattern": fre, "role": role, "nargs": min(nargs, 6), "matches": [],
               "absent": []}
        if not hits:
            row["absent"].append("entry")
            if "r" in kinds:
                row["absent"].append("return")
        for m, f in hits:
            have = probes[(m, f)]
            row["matches"].append({"module": m, "function": f, "entry": "entry" in have,
                                   "return": "r" in kinds and "return" in have})
            if "r" in kinds and "return" not in have:
                row["absent"].append(f"return:{f}")
        rows.append(row)
    return rows


def dscript(rows, use_proc=True, use_stack=True, hot_cap=50000, tick_s=240):
    def args(n):
        return " ".join(f"a{i}=0x%llx" for i in range(n)), ", ".join(f"(unsigned long long)arg{i}" for i in range(n))

    out = ["#pragma D option quiet", "#pragma D option bufsize=64m", "#pragma D option switchrate=20hz",
           "#pragma D option aggsize=16m", "#pragma D option dynvarsize=8m",
           "int seg; int hot;",
           'dtrace:::BEGIN { printf("TRACE-START w=%llu\\n", (unsigned long long)walltimestamp); }']
    if use_proc:
        out.append('proc:::exec-success /execname == "ane_inmem_run"/ { seg++; '
                   'printf("SEG w=%llu seg=%d pid=%d\\n", (unsigned long long)walltimestamp, seg, pid); }')
    n = 0
    for row in rows:
        for m in row["matches"]:
            p = f"fbt:{m['module']}:{m['function']}"
            role, na = row["role"], row["nargs"]
            gate = role == "gate" and m["return"]
            if role == "anchor":
                out.append(f'{p}:entry {{ printf("B w=%llu f=%s\\n", (unsigned long long)walltimestamp, probefunc); }}')
                n += 1
                continue
            if role == "write":
                fmt, vals = args(5)
                key = "probefunc, seg, arg1, arg2, arg3"
                out.append(f"{p}:entry {{ @n[{key}] = count(); @f[{key}] = min(walltimestamp); "
                           f"@l[{key}] = max(walltimestamp); }}")
                cond = "/self->d > 0 || hot < %d/" % hot_cap if row["tag"] != "PMGR" or "ANEClkGen" not in m["function"] else ""
                body = (f'hot++; printf("W w=%llu f=%s d=%d {fmt}\\n", (unsigned long long)walltimestamp, '
                        f"probefunc, self->d, {vals});")
                out.append(f"{p}:entry {cond} {{ {body} }}")
                if use_stack:
                    out.append(f"{p}:entry /self->d > 0/ {{ stack(3); }}")
                n += 2
                if m["return"]:
                    out.append(f'{p}:return /self->d > 0/ {{ printf("R w=%llu f=%s rv=0x%llx\\n", '
                               "(unsigned long long)walltimestamp, probefunc, (unsigned long long)arg1); }")
                    n += 1
                continue
            fmt, vals = args(na)
            pre = "self->d++; " if gate else ""
            out.append(f'{p}:entry {{ {pre}printf("E w=%llu f=%s {fmt}\\n", (unsigned long long)walltimestamp, '
                       f"probefunc, {vals}); }}")
            if use_stack:
                out.append(f"{p}:entry {{ stack(3); }}")
            n += 1
            if m["return"]:
                post = " self->d--;" if gate else ""
                out.append(f'{p}:return {{ printf("R w=%llu f=%s rv=0x%llx\\n", (unsigned long long)walltimestamp, '
                           f"probefunc, (unsigned long long)arg1);{post} }}")
                n += 1
    out.append(f"tick-{tick_s}s {{ exit(0); }}")
    out.append('dtrace:::END { printf("TRACE-END w=%llu hot=%d\\n", (unsigned long long)walltimestamp, hot); '
               'printa("A f=%s seg=%d a1=0x%x a2=0x%x a3=0x%x n=%@u first=%@u last=%@u\\n", @n, @f, @l); }')
    return "\n".join(out) + "\n", n


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("listing")
    ap.add_argument("--out", required=True)
    ap.add_argument("--probes", required=True)
    ap.add_argument("--no-proc", action="store_true")
    ap.add_argument("--no-stack", action="store_true")
    ap.add_argument("--hot-cap", type=int, default=50000)
    a = ap.parse_args()
    rows = resolve(listing(a.listing))
    text, n = dscript(rows, not a.no_proc, not a.no_stack, a.hot_cap)
    open(a.out, "w").write(text)
    present = sum(1 for r in rows if r["matches"])
    absent = [f"{r['tag']}:{r['pattern']} {r['absent']}" for r in rows if r["absent"]]
    json.dump({"targets": len(rows), "present": present, "clauses": n, "absent": absent, "rows": rows},
              open(a.probes, "w"), indent=1)
    print(f"targets={len(rows)} present={present} clauses={n}", file=sys.stderr)
    for s in absent:
        print(f"absent {s}", file=sys.stderr)
    return 0 if present else 1


if __name__ == "__main__":
    sys.exit(main())
