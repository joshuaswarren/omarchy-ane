#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren
"""Judge community collect rows against the README "Promotion" and "Regression" rules.

Reads published mlx-omarchy community rows (the JSON of /v1/results/<sha256>,
one row per file, or a JSONL file of such rows) and prints, per SoC, each
judged row with its failures and the verdict: PROMOTE, CONFLICT or STAY for an
opt-in SoC, ON or REVERT for a SoC whose overlay is enabled in
packaging/dt/overlays:

    tools/promotion_check.py ROW.json [ROW.json | ROWS.jsonl ...]
    tools/promotion_check.py --remote [URL]     # every row of the public dataset

A row is judged only when it carries the omarchy-ane block and provides
evidence: an unready check, an ANE fault, a wrong driver, or an attempted smoke.
An uninstalled row, a row whose driver_source is none (no driver for its
kernel), or a clean ready row with no smoke attempt, is not judged
and counts neither for nor against a chip. One passing
row promotes an opt-in SoC. A passing row has omarchy-ane-check ready, the
chip's driver loaded, 20 bit-exact smoke calls against the golden, and no
ANE/DART/mailbox fault line. The driver is the kernel's own
(driver_source=intree) or omarchy-ane-dkms's (dkms; a row without the field
is a dkms row); both count the same. A chip with both a passing and a failing
row is a CONFLICT: it does not promote until the failing row is explained or
superseded. A PROMOTE has targets: overlay (tools/promote_chip.py), plus
aurora-dt (tools/aurora_dt.py) when a passing row is intree.
Exit 0 always: the output is a report, not a gate.
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
# The community collector's field (omarchy-mlx scripts/collect_deep.py), also
# the omarchy-ane-check line: whose driver module the row ran.
DRIVER_SOURCE = "driver_source"
INTREE, DKMS, NONE = "intree", "dkms", "none"
# What a PROMOTE changes: the omarchy-ane overlay (tools/promote_chip.py) and,
# for an in-tree passing row, the kernel's own device tree (tools/aurora_dt.py).
OVERLAY, AURORA_DT = "overlay", "aurora-dt"
# The smoke golden per SoC: the add-fixture golden where omarchy-ane-smoke
# has a fixture. A SoC without one cannot pass a row.
GOLDEN = SMOKE.GOLDEN
MIN_SMOKE_CALLS = 20
# A line counts as an ANE line only when it names the ANE platform device
# (<addr>.ane, bare or with the driver prefix "ane"/"ane_t6021:"), one of the
# ANE DARTs (285800000/285810000/285820000 on T600x and T602x,
# 26b800000/26b810000/26b820000 on T8103 and T8112, per packaging/dt/)
# or the ANE mailbox (285408000 T602x, 26b408000 T8112). Other devices log
# look-alike lines (apple-dcp RTKit syslog chatter, other DARTs), and the
# collector's dmesg_faults list is not scoped to the ANE: a line decides a
# row only through this matcher, never by being listed.
ANE_LINE = re.compile(r"\b[0-9a-f]+\.ane\b|\bane(_t6021)?:"
                      r"|apple-dart (2858[0-2]0000|26b8[0-2]0000)\.iommu"
                      r"|apple-mailbox (285408000|26b408000)\.mailbox")
FAULT = re.compile(r"fault|error|fail(?:ed|ure)?|timed? ?out|abort|oops|warn|bug|call trace|stall|hung",
                   re.IGNORECASE)


def block(row):
    runtime = ((row.get("summary") or {}).get("ane_port_detail") or {}).get("runtime") or {}
    oa = runtime.get("omarchy_ane")
    return oa if isinstance(oa, dict) else None


def soc(row):
    chip = row.get("chip") or ""
    return chip.split(",", 1)[1] if chip.startswith("apple,t") else None


def installed(oa):
    """True unless the collector explicitly says omarchy-ane is unavailable."""
    return (oa.get("check") or {}).get("available") is not False


def smoke_attempted(smoke):
    """Whether the collector tried the smoke rather than skipping or deferring it."""
    if smoke.get("busy") or smoke.get("attempted") is False or smoke.get("requested") is False:
        return False
    if smoke.get("attempted") is True or smoke.get("requested") is True:
        return True
    return "sha256" in smoke or smoke.get("available") is True or (
        smoke.get("available") is False and "name" in smoke)


def driver_source(oa):
    """intree, dkms or none; a legacy row without the field is a dkms row."""
    return oa.get(DRIVER_SOURCE) or DKMS


def judged(row):
    """Rows with failure evidence count even when no smoke ran; clean skips do not."""
    oa, s = block(row), soc(row)
    if not oa or not installed(oa) or driver_source(oa) == NONE:
        return False
    module = (oa.get("module") or {}).get("name")
    return bool(unclean(oa)) or module != DRIVER.get(s) or smoke_attempted(oa.get("smoke") or {})


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
    faults = [l for l in (oa.get("dmesg_faults") or []) + (oa.get("dmesg") or [])
              if ANE_LINE.search(l) and FAULT.search(l)]
    if faults:
        faults = list(dict.fromkeys(faults))
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
    elif len(hashes) != MIN_SMOKE_CALLS or smoke.get("errors") or any(h != golden for h in hashes):
        bad = sum(h != golden for h in hashes)
        out.append(f"smoke {len(hashes)} calls, {bad} not bit-exact, {smoke.get('errors')} errors "
                   f"(want exactly {MIN_SMOKE_CALLS} bit-exact, 0 errors)")
    return out


def verdict(rows):
    """{soc: judged/passing/failing ids, promote/conflict, latest, revert}."""
    by_soc = defaultdict(list)
    for row in rows:
        if soc(row) and judged(row):
            by_soc[soc(row)].append(row)
    out = {}
    for s, judged_rows in sorted(by_soc.items()):
        ok = [r for r in judged_rows if not failures(r)]
        bad = [r for r in judged_rows if failures(r)]
        latest = max(judged_rows, key=lambda r: r.get("received_at") or "")
        out[s] = {"judged": len(judged_rows), "passing": [r["content_sha256"][:12] for r in ok],
                  "failing": {r["content_sha256"][:12]: failures(r) for r in bad},
                  "promote": bool(ok) and not bad, "conflict": bool(ok) and bool(bad),
                  "needs": ([f"{len(bad)} failing row(s) to explain"] if ok and bad else
                            ["one passing row"] if not ok else []),
                  "intree": [r["content_sha256"][:12] for r in ok if driver_source(block(r)) == INTREE],
                  "on": s in ON, "latest": latest["content_sha256"][:12],
                  "revert": unclean(block(latest)) if s in ON else []}
    return out


def targets(r):
    """What a PROMOTE or REVERT verdict changes."""
    if r["on"]:
        return [OVERLAY] if r["revert"] else []
    return ([OVERLAY] + ([AURORA_DT] if r["intree"] else [])) if r["promote"] else []


def unattempted(rows):
    """{soc: count of rows with no judgment evidence}."""
    out = defaultdict(int)
    for row in rows:
        if soc(row) and block(row) and not judged(row):
            out[soc(row)] += 1
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


def json_verdict(rows):
    """Stable machine verdict: {chips: [{chip, state, verdict, targets, rows: [...]}]}.

    verdict is PROMOTE | REVERT | STAY | ON | CONFLICT; targets is what the
    verdict changes (targets()); rows carry the 12-char row sha, the row's
    driver_source, whether the row was judged, and (judged rows) pass and reasons.
    """
    result, per_soc = verdict(rows), defaultdict(list)
    for row in rows:
        if soc(row):
            per_soc[soc(row)].append(row)
    chips = []
    for s in sorted(per_soc):
        r = result.get(s)
        rows_out = [{"row_sha": row["content_sha256"][:12], "judged": (j := judged(row)),
                     DRIVER_SOURCE: driver_source(block(row)) if block(row) else None,
                     "passed": (not failures(row)) if j else False,
                     "reasons": failures(row) if j else []} for row in per_soc[s]]
        if r is None:
            chips.append({"chip": s, "state": "on" if s in ON else "opt-in",
                          "verdict": "ON" if s in ON else "STAY", "targets": [], "rows": rows_out})
        elif r["on"]:
            chips.append({"chip": s, "state": "on", "verdict": "REVERT" if r["revert"] else "ON",
                          "targets": targets(r), "rows": rows_out})
        else:
            v = "PROMOTE" if r["promote"] else "CONFLICT" if r["conflict"] else "STAY"
            chips.append({"chip": s, "state": "opt-in", "verdict": v, "targets": targets(r), "rows": rows_out})
    return {"chips": chips}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("rows", nargs="*", help="row JSON or JSONL files")
    ap.add_argument("--remote", nargs="?", const=DATASET, help="read every row of the public dataset")
    ap.add_argument("--json", action="store_true", help="print the verdict as JSON instead of text")
    args = ap.parse_args(argv)
    if not args.rows and not args.remote:
        ap.error("give row files or --remote")
    rows = list(remote_rows(args.remote)) if args.remote else list(load(args.rows))
    if args.json:
        print(json.dumps(json_verdict(rows), indent=2))
        return 0
    per_soc = defaultdict(int)
    for row in rows:
        if soc(row):
            per_soc[soc(row)] += 1
    result = verdict(rows)
    unjudged = unattempted(rows)
    print(f"promotion_check: {len(rows)} rows; rule: one passing row promotes; a passing row has a ready check, "
          f"the chip driver, exactly 20 bit-exact smoke calls and no ANE/DART/mailbox fault; in-tree and dkms "
          f"driver rows count alike; uninstalled, driver_source none or clean no-smoke rows are not judged; "
          f"a passing and failing row conflict; on by default ({', '.join(sorted(ON))}): REVERT when the latest judged row is not clean")
    for s in sorted(per_soc):
        r = result.get(s)
        extra = f", {unjudged[s]} not judged (uninstalled, no driver or no smoke attempt)" if unjudged[s] else ""
        if r is None:
            print(f"{s}: {per_soc[s]} rows, 0 judged{extra} -> {'ON' if s in ON else 'STAY'}")
            continue
        if r["on"]:
            state = f"REVERT (latest judged row {r['latest']}: {'; '.join(r['revert'])})" if r["revert"] else "ON"
        else:
            label = "PROMOTE" if r["promote"] else "CONFLICT" if r["conflict"] else "STAY"
            state = label + (f" (needs: {'; '.join(r['needs'])})" if r["needs"] else "")
        if targets(r):
            state += f" -> targets: {', '.join(targets(r))}"
        print(f"{s}: {per_soc[s]} rows, {r['judged']} judged{extra}, {len(r['passing'])} pass, "
              f"{len(r['failing'])} fail -> {state}")
        for sha, why in r["failing"].items():
            print(f"  FAIL {sha}: {'; '.join(why)}")
        for sha in r["passing"]:
            print(f"  pass {sha}" + (f" ({INTREE})" if sha in r["intree"] else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
