#!/usr/bin/env python3
"""Host checks for the named-port runner contract."""

import json
import subprocess
import tempfile
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import numpy as np

from qwen_prog_run import Refuse, main, pack_surface, port_map_from_table, unpack_surface


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


def test_scratch_entry_needs_coverage_and_is_never_named():
    table = json.loads((ROOT / "tests/fixtures/prog020-ports.json").read_text())
    scratch = {"name": "scratch", "direction": "scratch", "surface_bytes": 16384,
               "tile_bytes": 16384, "bar_slot": 3, "buffer_id": 64}
    covered = dict(table, ports=table["ports"] + [scratch], dma_coverage=table["dma_coverage"] + [
        {"bar_slot": 3, "buffer_id": 64, "kind": "scratch", "port": "scratch"}])
    assert "scratch" not in port_map_from_table(covered)
    try:
        port_map_from_table(dict(covered, dma_coverage=table["dma_coverage"]))
    except Refuse:
        pass
    else:
        raise AssertionError("scratch without slot-3 coverage was accepted")


def test_pack_follows_padded_descriptor_strides():
    arr = np.arange(12, dtype=np.float16).reshape(1, 2, 2, 3)
    surf = pack_surface(arr, [256, 128, 64, 2], 512)
    assert surf[64 + 32 + 2] == arr[0, 1, 1, 2]  # plane 1, row 1, element 2
    assert np.count_nonzero(surf) == np.count_nonzero(arr)
    assert np.array_equal(unpack_surface(surf.tobytes(), arr.shape, [256, 128, 64, 2]), arr)
    for strides, size in (([256, 128, 4, 2], 512), ([256, 128, 64, 2], 128)):
        try:
            pack_surface(arr, strides, size)
        except Refuse:
            pass
        else:
            raise AssertionError(f"overlapping or oversized strides {strides} in {size} B accepted")


def test_live_run_refuses_programs_past_the_qualified_task_count():
    """A 20-task program must not reach ane-run until the driver waits for
    the program's own TD count; --dry still gets past the gate."""
    with tempfile.TemporaryDirectory() as tmp:
        anec = Path(tmp) / "prog_020" / "program-0.anec"
        anec.parent.mkdir()
        anec.write_bytes(bytes(12) + (20).to_bytes(4, "little"))
        argv = ["--prog", "prog_020", "--anec-dir", tmp, "--ane-run", "/nonexistent/ane-run",
                "--ports", str(ROOT / "tests/fixtures/prog020-ports.json")]
        for extra, gated in (([], True), (["--dry"], False)):
            try:
                main(argv + extra)
            except Refuse as err:
                assert ("has 20 tasks" in str(err)) == gated, err
            else:
                raise AssertionError("run without inputs was accepted")


def test_ane_run_refuses_surface_larger_than_tile():
    """The port table sizes the io BOs; a surface that does not fit would
    let the task DMA run past its BO. Needs tools/ane-run built."""
    table = json.loads((ROOT / "tests/fixtures/prog020-ports.json").read_text())
    with tempfile.TemporaryDirectory() as tmp:
        for surface, refused in ((4096, False), (16386, True)):
            table["ports"][0]["surface_bytes"] = surface
            path = Path(tmp) / "ports.json"
            path.write_text(json.dumps(table))
            run = subprocess.run([str(ROOT / "tools/ane-run"), "--anec", "/nonexistent.anec",
                                  "--ports", str(path), "--dry-run"],
                                 capture_output=True, text=True)
            assert run.returncode == 1
            assert ("larger than tile_bytes" in run.stderr) == refused, run.stderr
            assert ("failed to open /nonexistent.anec" in run.stderr) == (not refused), run.stderr


if __name__ == "__main__":
    test_prog020_table_contract()
    test_duplicate_or_missing_dma_coverage_refuses()
    test_ane_run_refuses_surface_larger_than_tile()
    test_scratch_entry_needs_coverage_and_is_never_named()
    test_pack_follows_padded_descriptor_strides()
    test_live_run_refuses_programs_past_the_qualified_task_count()
    print("named-port runner checks passed")
