#!/usr/bin/env python3
"""docs/ane-probe.md must name every top-level key the probe emits.

Runs tools/omarchy-ane-probe with --root on an empty directory (fast, no
dtc, no fixtures) and fails when an emitted top-level key has no
backquoted mention in the doc. New keys belong in the "What it reads"
table; that is where reviewers look first.
"""
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
TOOL = REPO / "tools/omarchy-ane-probe"
DOC = REPO / "docs/ane-probe.md"


def emitted_keys():
    with tempfile.TemporaryDirectory() as root:
        out = subprocess.run(
            [sys.executable, str(TOOL), "--root", root],
            check=True, capture_output=True, text=True,
        ).stdout
    return set(json.loads(out))


def documented_words():
    return set(re.findall(r"`([a-z_]+)`", DOC.read_text()))


def main():
    missing = emitted_keys() - documented_words()
    if missing:
        print("ane-probe emits keys the doc does not name:")
        for key in sorted(missing):
            print(f"  {key}")
        print("add a row for each to docs/ane-probe.md")
        return 1
    print("test_ane_probe_doc: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
