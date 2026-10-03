#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren
"""Generate the window-3 D script from the live box's own `dtrace -l -P fbt` listing (T6021 and T8103).

  w3gen.py FBT_LISTING --out w3.d --probes probes.json [--execname a,b] [--tick-s N]
                       [--no-proc] [--no-stack] [--hot-cap N]

Every clause names a probe that exists in the listing (a missing name stops dtrace). Targets are
matched by regex on the mangled function name, `.cold.N` split-offs excluded. A target whose return
probe is absent gets its entry clause only and is listed under "absent"; its thread gate is then not
used (an entry-only gate would never clear). The SoC subclass rows match AppleT60xxPMGR (T6021) and
AppleT8103PMGR / AppleT810xPMGR (T8103) in any PMGR module.

Output lines (quiet mode):
  E w=<ns> f=<fn> a0=.. a5=..     entry of an ANE-path function; kernel stack frames follow, indented
  R w=<ns> f=<fn> rv=0x..          return value (arg1) of an ANE-path function
  W w=<ns> f=<fn> d=<gate depth> a0=.. a4=..   one register write (ANEClkGen or writeReg32)
  RD w=<ns> f=<fn> d=<depth> a0=.. a3=..       a readReg32 inside an ANE-path gate (its R line follows)
  P w=<ns> f=<fn> d=<depth> a0=<this> cmd=.. p=<payload ptr> n=<count>   one _sendPMPCommand
  PV / PRV w=<ns> f=<fn> i=<k> v=..   payload word k (u64) at entry / at return
  B w=<ns> f=<fn>                  aneWorkBegin / aneWorkEnd anchors
  SEG w=<ns> seg=<n> pid=<p> exe=<name>   a runner exec (proc provider), numbers the ramps
  A f=<fn> seg=<n> a1=.. a2=.. a3=.. n=<count> first=<ns> last=<ns>   END aggregation of every write
  PA f=<fn> seg=<n> cmd=.. n=<count>   END aggregation of every PMP command
`w` is walltimestamp (ns since the epoch, the clock w2sample.py stamps events with). For a C++ member
function arg0 is `this`, so ApplePMGR::writeReg32(RegMap, offset, value, die) is a1..a4.
"""
import argparse
import json
import re
import sys

SOC = r"AppleT(?:60\d\d|8\d\d[\dx])PMGR\d+"
# tag, module regex, function regex (mangled), args to print (this + params, max 6), kinds, role
# roles: gate = sets self->d on entry, clears on return (needs both probes); write = per-event +
# aggregate; read = per-event only inside a gate; pmp = gate + command/payload capture;
# anchor = begin/end markers (one probe: the shortest match of the first pattern that matches).
TARGETS = [
    ("CLPC", r"CLPC", r"clpc3ane10DVFManager\d+requestPerformanceChange", 2, "er", "gate"),
    ("CLPC", r"CLPC", r"clpc3ane10DVFManager6enable", 2, "er", "gate"),
    ("CLPC", r"CLPC", r"clpc3ane11JobTracking\d+getActiveJobRequestedOperatingState", 3, "er", "gate"),
    ("CLPC", r"CLPC", r"PMCVoterInterface\d+setANEPerfStateFloor", 3, "er", "gate"),
    ("CLPC", r"CLPC", r"PMCVoterInterface\d+writePerfStateFloorReg", 2, "er", "gate"),
    ("CLPC", r"CLPC", r"PMCVoterInterface\d+writeRegValue", 1, "er", "gate"),
    ("PMGR", r"ApplePMGR$", r"ApplePMGRNub\d+requestPerfState", 3, "er", "gate"),
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
    ("PMGR", r"ApplePMGR$", r"ApplePMGR\d+_sendPMPCommand", 4, "er", "pmp"),
    ("PMGR", r"ApplePMGR$", r"ApplePMGRv2\d+_sendPMPCommand", 4, "er", "pmp"),
    ("PMGR", r"ApplePMGR$", r"tracePerfStateChange", 3, "er", "gate"),
    ("PMC", r"ApplePMGR$", r"ApplePMC\d+setPerfStateAgent", 6, "er", "gate"),
    ("PMC", r"ApplePMGR$", r"ApplePMC\d+writeReg32", 4, "e", "write"),
    ("PMGR", r"ApplePMGR$", r"ApplePMGR\d+writeReg32", 5, "e", "write"),
    ("PMGR", r"ApplePMGR$", r"ApplePMGR\d+readReg32", 4, "er", "read"),
    ("SOC", r"PMGR", SOC + r"setPerfState", 5, "er", "gate"),
    ("SOC", r"PMGR", SOC + r"_setPerfState", 4, "er", "gate"),
    ("SOC", r"PMGR", SOC + r"_handlePerfStateRequest", 4, "er", "gate"),
    ("SOC", r"PMGR", SOC + r"writeReg32", 5, "e", "write"),
    ("SOC", r"PMGR", SOC + r"readReg32", 4, "er", "read"),
    ("SOC", r"PMGR", SOC + r"getFANEIndex", 4, "er", "gate"),
    ("ANCHOR", r"CLPC", (r"clpc4CLPC\d+aneWorkBegin", r"aneWorkBegin"), 0, "e", "anchor"),
    ("ANCHOR", r"CLPC", (r"clpc4CLPC\d+aneWorkEnd", r"aneWorkEnd"), 0, "e", "anchor"),
]
PMP_WORDS = 4


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
        pats = fre if isinstance(fre, tuple) else (fre,)
        hits = []
        for pat in pats:
            hits = sorted((m, f) for (m, f) in probes if re.search(mre, m) and re.search(pat, f)
                          and "block_invoke" not in f)
            if hits:
                break
        if role == "anchor" and hits:
            hits = [min(hits, key=lambda mf: (len(mf[1]), mf))]
        row = {"tag": tag, "pattern": "|".join(pats), "role": role, "nargs": min(nargs, 6), "matches": [],
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


def dscript(rows, execnames=("ane_inmem_run",), use_proc=True, use_stack=True, hot_cap=50000, tick_s=240):
    w = "(unsigned long long)walltimestamp"

    def args(n):
        return " ".join(f"a{i}=0x%llx" for i in range(n)), ", ".join(f"(unsigned long long)arg{i}" for i in range(n))

    out = ["#pragma D option quiet", "#pragma D option bufsize=64m", "#pragma D option switchrate=20hz",
           "#pragma D option aggsize=16m", "#pragma D option dynvarsize=8m",
           "int seg; int hot;",
           f'dtrace:::BEGIN {{ printf("TRACE-START w=%llu\\n", {w}); }}']
    if use_proc:
        pred = " || ".join(f'execname == "{e}"' for e in execnames)
        out.append(f"proc:::exec-success /{pred}/ {{ seg++; "
                   f'printf("SEG w=%llu seg=%d pid=%d exe=%s\\n", {w}, seg, pid, execname); }}')
    n = 0
    for row in rows:
        for m in row["matches"]:
            p = f"fbt:{m['module']}:{m['function']}"
            role, na = row["role"], row["nargs"]
            gate = role in ("gate", "pmp") and m["return"]
            if role == "anchor":
                out.append(f'{p}:entry {{ printf("B w=%llu f=%s\\n", {w}, probefunc); }}')
                n += 1
                continue
            if role == "write":
                fmt, vals = args(5)
                key = "probefunc, seg, arg1, arg2, arg3"
                out.append(f"{p}:entry {{ @n[{key}] = count(); @f[{key}] = min(walltimestamp); "
                           f"@l[{key}] = max(walltimestamp); }}")
                cond = "" if "ANEClkGen" in m["function"] else "/self->d > 0 || hot < %d/" % hot_cap
                out.append(f'{p}:entry {cond} {{ hot++; printf("W w=%llu f=%s d=%d {fmt}\\n", {w}, '
                           f"probefunc, self->d, {vals}); }}")
                n += 2
                if use_stack:
                    out.append(f"{p}:entry /self->d > 0/ {{ stack(3); }}")
                    n += 1
                if m["return"]:
                    out.append(f'{p}:return /self->d > 0/ {{ printf("R w=%llu f=%s rv=0x%llx\\n", {w}, '
                               "probefunc, (unsigned long long)arg1); }")
                    n += 1
                continue
            if role == "read":
                fmt, vals = args(na)
                out.append(f'{p}:entry /self->d > 0/ {{ self->rd = 1; printf("RD w=%llu f=%s d=%d {fmt}\\n", {w}, '
                           f"probefunc, self->d, {vals}); }}")
                n += 1
                if m["return"]:
                    out.append(f'{p}:return /self->rd/ {{ self->rd = 0; printf("R w=%llu f=%s rv=0x%llx\\n", {w}, '
                               "probefunc, (unsigned long long)arg1); }")
                    n += 1
                continue
            fmt, vals = args(na)
            pre = "self->d++; " if gate else ""
            if role == "pmp":
                out.append(f"{p}:entry {{ {pre}self->pp = arg2; self->pn = arg3; @pmp[probefunc, seg, arg1] = count(); "
                           f'printf("P w=%llu f=%s d=%d a0=0x%llx cmd=0x%llx p=0x%llx n=%llu\\n", {w}, probefunc, '
                           "self->d, (unsigned long long)arg0, (unsigned long long)arg1, (unsigned long long)arg2, "
                           "(unsigned long long)arg3); }")
                n += 1
                for k in range(PMP_WORDS):
                    out.append(f'{p}:entry /arg2 != 0 && arg3 > {k}/ {{ printf("PV w=%llu f=%s i={k} v=0x%llx\\n", {w}, '
                               f"probefunc, *(uint64_t *)(arg2 + {8 * k})); }}")
                    n += 1
            else:
                out.append(f'{p}:entry {{ {pre}printf("E w=%llu f=%s {fmt}\\n", {w}, probefunc, {vals}); }}')
                n += 1
            if use_stack:
                out.append(f"{p}:entry {{ stack(3); }}")
                n += 1
            if m["return"]:
                if role == "pmp":
                    for k in range(PMP_WORDS):
                        out.append(f'{p}:return /self->pp != 0 && self->pn > {k}/ {{ printf("PRV w=%llu f=%s i={k} '
                                   f'v=0x%llx\\n", {w}, probefunc, *(uint64_t *)(self->pp + {8 * k})); }}')
                        n += 1
                post = " self->d--;" if gate else ""
                clear = " self->pp = 0; self->pn = 0;" if role == "pmp" else ""
                out.append(f'{p}:return {{ printf("R w=%llu f=%s rv=0x%llx\\n", {w}, '
                           f"probefunc, (unsigned long long)arg1);{post}{clear} }}")
                n += 1
    out.append(f"tick-{tick_s}s {{ exit(0); }}")
    body = "\n".join(out)
    pa = ('printa("A f=%s seg=%d a1=0x%x a2=0x%x a3=0x%x n=%@u first=%@u last=%@u\\n", @n, @f, @l); '
          if "@n[" in body else "")  # an aggregation printed but never used is a compile error
    pp = 'printa("PA f=%s seg=%d cmd=0x%x n=%@u\\n", @pmp); ' if "@pmp[" in body else ""
    out.append(f'dtrace:::END {{ printf("TRACE-END w=%llu hot=%d\\n", {w}, hot); {pa}{pp}}}')
    return "\n".join(out) + "\n", n


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("listing")
    ap.add_argument("--out", required=True)
    ap.add_argument("--probes", required=True)
    ap.add_argument("--execname", default="ane_inmem_run", help="comma list of runner process names")
    ap.add_argument("--tick-s", type=int, default=240)
    ap.add_argument("--no-proc", action="store_true")
    ap.add_argument("--no-stack", action="store_true")
    ap.add_argument("--hot-cap", type=int, default=50000)
    a = ap.parse_args()
    names = [e for e in a.execname.split(",") if e]
    if not names or any(not re.fullmatch(r"[A-Za-z0-9_.-]+", e) for e in names):
        ap.error("--execname: comma list of plain process names")
    rows = resolve(listing(a.listing))
    text, n = dscript(rows, names, not a.no_proc, not a.no_stack, a.hot_cap, a.tick_s)
    open(a.out, "w").write(text)
    present = sum(1 for r in rows if r["matches"])
    absent = [f"{r['tag']}:{r['pattern']} {r['absent']}" for r in rows if r["absent"]]
    json.dump({"targets": len(rows), "present": present, "clauses": n, "execnames": names, "absent": absent,
               "rows": rows}, open(a.probes, "w"), indent=1)
    print(f"targets={len(rows)} present={present} clauses={n}", file=sys.stderr)
    for s in absent:
        print(f"absent {s}", file=sys.stderr)
    return 0 if present else 1


if __name__ == "__main__":
    sys.exit(main())
