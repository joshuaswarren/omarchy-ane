#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate saved diagnostic snapshots, not hardware execution or rung success."""
import argparse
import json
from pathlib import Path
import re


CANDIDATES = {0x10000000000, 0x3FFFFBB0800, 0xFFBB0800}
DEVICES = {"401800000.iommu", "401820000.iommu", "401840000.iommu"}
RESULTS = {"published", "deferred-missing-root", "missing-root",
           "incomplete-root", "legacy-copy", "publish-error"}
WORD = r"(?:0|0x[0-9a-f]{1,16})"
LINE = re.compile(
    r"apple-dart (?P<device>[a-z0-9.]+): h17-pub "
    r"seq=(?P<seq>[0-9]+) result=(?P<result>[a-z-]+) "
    r"deferred=(?P<deferred>[01]) ret=(?P<ret>-?[0-9]+) "
    r"levels=(?P<levels>[0-9]+) ttbrs=(?P<ttbrs>[0-9]+) "
    rf"dva=(?P<dva>{WORD}) index=(?P<index>[0-9]+) slot=(?P<slot>[0-9]+) "
    rf"present=(?P<present>{WORD}) host=(?P<host>{WORD}) "
    rf"owned=(?P<owned>{WORD}) live=(?P<live>{WORD})$")


def analyze(text, page_shift):
    """page_shift is an explicit caller assumption, not measured by this log."""
    report = {"capture": "INVALID", "hardware_qualification": False,
              "assumed_page_shift": page_shift, "snapshots": []}
    try:
        if page_shift not in (12, 14):
            raise ValueError("unsupported page-shift assumption")
        bits = page_shift - 3
        entries = 1 << bits
        groups = {}
        for line in text.splitlines():
            if "h17-pub " not in line:
                continue
            match = LINE.search(line)
            if not match:
                raise ValueError("malformed publication record")
            row = match.groupdict()
            for key in row.keys() - {"device", "result"}:
                row[key] = int(row[key], 16 if key in
                               {"dva", "present", "host", "owned", "live"} else 10)
            if row["device"] not in DEVICES or row["result"] not in RESULTS:
                raise ValueError("unexpected device or sync result")
            if not 1 <= row["seq"] <= 64:
                raise ValueError("sequence outside diagnostic bound")
            if row["levels"] not in (3, 4) or not 1 <= row["ttbrs"] <= 4:
                raise ValueError("unsupported software table geometry")
            if row["dva"] not in CANDIDATES or row["present"] > 7:
                raise ValueError("unexpected candidate or presence mask")
            index = 0 if row["levels"] == 4 else row["dva"] >> (page_shift + 2 * bits)
            slot = (row["dva"] >> (page_shift + (row["levels"] - 2) * bits)) & (entries - 1)
            if (row["index"], row["slot"]) != (index, slot):
                raise ValueError("index/slot contradict page-shift assumption")
            if index >= min(4, row["ttbrs"]) and row["present"]:
                raise ValueError("presence reported outside table bounds")
            for bit, key in enumerate(("host", "owned", "live")):
                if not row["present"] & (1 << bit) and row[key]:
                    raise ValueError("word supplied for absent table")
            if row["present"] & 2 and not row["present"] & 4:
                raise ValueError("ownership sampled without a live root")
            if ((row["result"] in {"incomplete-root", "publish-error"}) !=
                    (row["ret"] < 0)) or row["ret"] > 0:
                raise ValueError("sync result contradicts return code")
            if row["result"] == "deferred-missing-root" and not row["deferred"]:
                raise ValueError("deferred result without deferred sync")
            group = groups.setdefault((row["device"], row["seq"]), [])
            if any(prior["dva"] == row["dva"] for prior in group):
                raise ValueError("duplicate candidate in snapshot")
            if group and any(row[k] != group[0][k] for k in
                             ("result", "deferred", "ret", "levels", "ttbrs")):
                raise ValueError("inconsistent snapshot metadata")
            if any(prior["index"] == row["index"] and
                   prior["present"] != row["present"] for prior in group):
                raise ValueError("inconsistent presence for a shared root")
            if index >= min(4, row["ttbrs"]):
                row["relation"] = "outside-software-table-bounds"
            elif row["present"] != 7:
                row["relation"] = "table-state-unavailable"
            elif row["host"] == row["owned"] == row["live"]:
                row["relation"] = "all-words-equal" if row["host"] else "all-words-zero"
            else:
                row["relation"] = "words-differ"
            group.append(row)
        if not groups:
            raise ValueError("no diagnostic snapshots")
        for device in sorted({key[0] for key in groups}):
            sequences = sorted(key[1] for key in groups if key[0] == device)
            if sequences != list(range(1, sequences[-1] + 1)):
                raise ValueError("missing initial or intermediate snapshot")
        for key, rows in sorted(groups.items()):
            if len(rows) != 3:
                raise ValueError("incomplete three-candidate snapshot")
            report["snapshots"].append({"device": key[0], "seq": key[1],
                                         "candidates": rows})
        report.update(capture="PARSED", reason="software-root-observations-only",
                      devices_seen=sorted({key[0] for key in groups}),
                      devices_missing=sorted(DEVICES - {key[0] for key in groups}),
                      limit_reached=sorted({dev for dev, seq in groups if seq == 64}))
    except ValueError as exc:
        report.update(reason=str(exc), snapshots=[])
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--page-shift", type=int, required=True, choices=(12, 14),
                        help="explicit software geometry assumption; not a device measurement")
    args = parser.parse_args()
    result = analyze(args.log.read_text(), args.page_shift)
    print(json.dumps(result, sort_keys=True))
    raise SystemExit(0 if result["capture"] == "PARSED" else 1)
