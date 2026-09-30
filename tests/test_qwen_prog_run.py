#!/usr/bin/env python3
"""Host checks for the named-port runner contract."""

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from qwen_prog_run import Refuse, port_map_from_table


def test_prog020_table_contract():
    table = json.loads((ROOT / "tests/fixtures/prog020-ports.json").read_text())
    ports = port_map_from_table(table)
    assert [ports[name]["channel"] for name in ("t0", "t2", "t7")] == [5, 6, 7]
    assert ports["t15"]["direction"] == "output"
    assert [ports[name]["bar_slot"] for name in ("t0", "t15", "t2", "t7")] == [4, 5, 6, 7]


def test_duplicate_or_missing_dma_coverage_refuses():
    table = json.loads((ROOT / "tests/fixtures/prog020-ports.json").read_text())
    slot = next(entry for entry in table["dma_coverage"] if entry["bar_slot"] == 4)
    try:
        port_map_from_table(dict(table, dma_coverage=table["dma_coverage"] + [slot]))
    except Refuse:
        pass
    else:
        raise AssertionError("duplicate slot coverage was accepted")
    try:
        port_map_from_table(dict(table, dma_coverage=[
            entry for entry in table["dma_coverage"] if entry["bar_slot"] != 4]))
    except Refuse:
        pass
    else:
        raise AssertionError("missing port slot coverage was accepted")


if __name__ == "__main__":
    test_prog020_table_contract()
    test_duplicate_or_missing_dma_coverage_refuses()
    print("named-port runner checks passed")
