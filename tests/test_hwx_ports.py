#!/usr/bin/env python3
"""Host checks for HWX port-table DMA coverage."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from hwx_ports import audit_dma_coverage


def test_exact_port_or_scratch_coverage():
    ports = [{"bar_slot": 4, "name": "x"}, {"bar_slot": 5, "name": "y"}]
    assert audit_dma_coverage([4, 5, 7], ports, {7: "scratch"}) == []
    assert audit_dma_coverage([4, 5], [*ports, {"bar_slot": 5, "name": "z"}], {}) == [
        "slot 5 is covered by 2 ports/surfaces"
    ]
    assert audit_dma_coverage([4, 6], ports, {}) == [
        "slot 6 is uncovered"
    ]


if __name__ == "__main__":
    test_exact_port_or_scratch_coverage()
    print("HWX port coverage checks passed")
