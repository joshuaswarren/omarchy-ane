#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren
"""Judge community collect rows against the README "Promotion" and "Regression" rules.

Reads published mlx-omarchy community rows (the JSON of /v1/results/<sha256>,
one row per file, or a JSONL file of such rows) and prints, per SoC, each
judged row with its failures and the verdict: PROMOTE or STAY for an opt-in
SoC, ON or REVERT for a SoC whose overlay is enabled in packaging/dt/overlays:

    tools/promotion_check.py ROW.json [ROW.json | ROWS.jsonl ...]
    tools/promotion_check.py --remote [URL]     # every row of the public dataset

A row is judged only when it carries the omarchy-ane block
(summary.ane_port_detail.runtime.omarchy_ane). Exit 0 always: the output is
a report, not a gate.
"""
import argparse
import json
import re
import sys
import urllib.request
from collections import defaultdict
from importlib.machinery import SourceFileLoader
from importlib.util import module_from_spec, spec_from_loader
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True  # no __pycache__ beside the extensionless packaging/omarchy-ane-smoke
_spec = spec_from_loader("smoke", SourceFileLoader("smoke", str(REPO / "packaging/omarchy-ane-smoke")))
SMOKE = module_from_spec(_spec)
_spec.loader.exec_module(SMOKE)

DATASET = "https://mlx-omarchy-community-data.joshua-s-warren.workers.dev"
# Cloudflare bot protection refuses the default Python user agent.
USER_AGENT = "omarchy-ane-promotion-check/1 (+https://github.com/joshuaswarren/omarchy-ane)"

DRIVER = {"t8103": "ane", "t6000": "ane", "t6001": "ane", "t6002": "ane",
          "t6020": "ane_t6021", "t6021": "ane_t6021", "t6022": "ane_t6021",
          "t8112": "ane_t6021"}
# The smoke golden per SoC: the add-fixture golden where omarchy-ane-smoke
# has a fixture. A SoC without one cannot pass a row.
GOLDEN = SMOKE.GOLDEN
# Board device trees per SoC in linux-asahi 7.1.13-3 (arch/arm64/boot/dts/apple).
BOARDS = {"t8103": 5, "t6000": 2, "t6001": 3, "t6002": 1, "t6020": 3,
          "t6021": 3, "t6022": 2, "t8112": 4}

N_ROWS, N_MACHINES, N_OWNERS, N_KERNELS = 3, 3, 2, 2
MIN_UPTIME_S, MIN_SMOKE_CALLS = 1800, 20
ANE_LINE = re.compile(r"\bane(_t6021)?\b|\bane@|\.ane\b|apple-dart.*(2858[0-2]0000|26b8[0-2]0000)"
                      r"|apple-mailbox.*(285408000|26b408000)|mailbox@(285408000|26b408000)")
FAULT = re.compile(r"fault|error|timed? ?out|abort|oops|warn|bug|call trace|stall|hung",
                   re.IGNORECASE)


def block(row):
    runtime = ((row.get("summary") or {}).get("ane_port_detail") or {}).get("runtime") or {}
    oa = runtime.get("omarchy_ane")
    return oa if isinstance(oa, dict) else None


def board(row):
    dt = ((row.get("summary") or {}).get("ane_port_detail") or {}).get("devicetree") or {}
    compat = (dt.get("boot") or {}).get("compatible") or []
    return compat[0] if compat else None


def soc(row):
    chip = row.get("chip") or ""
    return chip.split(",", 1)[1] if chip.startswith("apple,t") else None


def default_on():
    """SoCs whose ANE overlay is enabled in packaging/dt/overlays."""
    lines = (REPO / "packaging/dt/overlays").read_text().splitlines()
    return {p for p, src, state in (l.split() for l in lines if l and l[0] != "#")
            if src == f"{p}-ane.dts" and state == "enabled"}


ON = default_on()


def unclean(oa):
    """Reasons a row is not clean (the regression rule): check not ready, or fault lines."""
    out = []
    check = oa.get("check") or {}
    if check.get("exit") != 0 or check.get("status") != "ready":
        out.append(f"omarchy-ane-check not ready (exit {check.get('exit')}, {check.get('status')})")
    faults = list(oa.get("dmesg_faults") or [])
    faults += [l for l in oa.get("dmesg") or [] if ANE_LINE.search(l) and FAULT.search(l)
               and l not in faults]
    if faults:
        out.append(f"{len(faults)} ANE/DART/mailbox fault line(s): {faults[0][:120]}")
    return out


def failures(row):
    """Reasons a judged row fails; empty list = the row passes."""
    s, oa = soc(row), block(row)
    out = unclean(oa)
    module = (oa.get("module") or {}).get("name")
    if module != DRIVER.get(s):
        out.append(f"module {module}, want {DRIVER.get(s)}")
    smoke = oa.get("smoke") or {}
    hashes = smoke.get("sha256") or []
    golden = GOLDEN.get(s)
    if golden is None:
        out.append(f"no smoke golden for {s}")
    elif len(hashes) < MIN_SMOKE_CALLS or smoke.get("errors") or any(h != golden for h in hashes):
        bad = sum(h != golden for h in hashes)
        out.append(f"smoke {len(hashes)} calls, {bad} not bit-exact, {smoke.get('errors')} errors "
                   f"(want {MIN_SMOKE_CALLS} bit-exact, 0 errors)")
    if (oa.get("uptime_s") or 0) < MIN_UPTIME_S:
        out.append(f"uptime {oa.get('uptime_s')} s < {MIN_UPTIME_S} s")
    if not board(row):
        out.append("no board compatible")
    return out


def verdict(rows):
    """{soc: dict(counts, passing/failing row ids, promote, needs, on, latest, revert)} over judged rows."""
    by_soc = defaultdict(list)
    for row in rows:
        if soc(row) and block(row):
            by_soc[soc(row)].append(row)
    out = {}
    for s, judged in sorted(by_soc.items()):
        ok = [r for r in judged if not failures(r)]
        bad = [r for r in judged if failures(r)]
        machines = {block(r).get("machine_id") for r in ok} - {None}
        owners = {block(r).get("owner_id") for r in ok} - {None}
        boards = {board(r) for r in ok}
        kernels = {r.get("kernel") for r in ok} - {None}
        want_boards = min(2, BOARDS.get(s, 1))
        needs = []
        for have, want, what in ((len(ok), N_ROWS, "passing rows"), (len(machines), N_MACHINES, "machines"),
                                 (len(owners), N_OWNERS, "owners"), (len(boards), want_boards, "boards"),
                                 (len(kernels), N_KERNELS, "kernel releases")):
            if have < want:
                needs.append(f"{what} {have}/{want}")
        if bad:
            needs.append(f"{len(bad)} failing row(s) to explain")
        latest = max(judged, key=lambda r: r.get("received_at") or "")
        out[s] = {"judged": len(judged), "passing": [r["content_sha256"][:12] for r in ok],
                  "failing": {r["content_sha256"][:12]: failures(r) for r in bad},
                  "machines": len(machines), "owners": len(owners), "boards": len(boards),
                  "kernels": len(kernels), "promote": not needs, "needs": needs,
                  "on": s in ON, "latest": latest["content_sha256"][:12],
                  "revert": unclean(block(latest)) if s in ON else []}
    return out


def fetch(url):
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=60) as resp:
        return resp.read().decode("utf-8")


def remote_rows(base):
    base = base.rstrip("/")
    for line in fetch(f"{base}/v1/dataset/latest.jsonl").splitlines():
        if line.strip():
            yield json.loads(fetch(f"{base}/v1/results/{json.loads(line)['content_sha256']}"))


def load(paths):
    for path in paths:
        text = open(path, encoding="utf-8").read()
        try:
            yield json.loads(text)
        except json.JSONDecodeError:
            yield from (json.loads(l) for l in text.splitlines() if l.strip())


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("rows", nargs="*", help="row JSON or JSONL files")
    ap.add_argument("--remote", nargs="?", const=DATASET, help="read every row of the public dataset")
    args = ap.parse_args(argv)
    if not args.rows and not args.remote:
        ap.error("give row files or --remote")
    rows = list(remote_rows(args.remote)) if args.remote else list(load(args.rows))
    per_soc = defaultdict(int)
    for row in rows:
        if soc(row):
            per_soc[soc(row)] += 1
    result = verdict(rows)
    print(f"promotion_check: {len(rows)} rows; rule: {N_ROWS} passing rows, {N_MACHINES} machines, "
          f"{N_OWNERS} owners, 2 boards (1 if the SoC has one), {N_KERNELS} kernel releases, 0 failing rows; "
          f"on by default ({', '.join(sorted(ON))}): REVERT when the latest row is not clean")
    for s in sorted(per_soc):
        r = result.get(s)
        if r is None:
            print(f"{s}: {per_soc[s]} rows, 0 judged (no omarchy-ane block) -> {'ON' if s in ON else 'STAY'}")
            continue
        if r["on"]:
            state = f"REVERT (latest row {r['latest']}: {'; '.join(r['revert'])})" if r["revert"] else "ON"
        else:
            state = ("PROMOTE" if r["promote"] else "STAY") + \
                (f" (needs: {'; '.join(r['needs'])})" if r["needs"] else "")
        print(f"{s}: {per_soc[s]} rows, {r['judged']} judged, {len(r['passing'])} pass, "
              f"{len(r['failing'])} fail, machines {r['machines']}, owners {r['owners']}, "
              f"boards {r['boards']}, kernels {r['kernels']} -> {state}")
        for sha, why in r["failing"].items():
            print(f"  FAIL {sha}: {'; '.join(why)}")
        for sha in r["passing"]:
            print(f"  pass {sha}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
