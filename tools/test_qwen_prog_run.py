#!/usr/bin/env python3
"""Self-test for tools/qwen_prog_run.py: the staged-program runner.

Exercises the no-device surface: stride validation and views, surface
pack/unpack round-trip (numpy path pinned with SURFACE_PACK=0 — the C
fast path has its own test), state_runs' contiguous-run logic, the
port-table validation, load_input's npy/raw/transpose cases, and
ane_call(dry=True) — which packs the input surfaces and returns the
flock command without any device access.
"""
import importlib.util
import os
import sys
import tempfile
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[1]

os.environ["SURFACE_PACK"] = "0"              # pin the numpy path; surface_pack has its own test
sys.path.insert(0, str(REPO / "tools"))
spec = importlib.util.spec_from_file_location("qwen_prog_run", REPO / "tools/qwen_prog_run.py")
qpr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qpr)

fails = []


def check(name, got, want):
    ok = np.array_equal(got, want) if isinstance(want, np.ndarray) else got == want
    if not ok:
        fails.append(f"{name}: wanted {want}, got {got}")
        return
    print(f"ok   {name}")


def refuses(name, fn):
    try:
        fn()
        fails.append(f"{name}: no refusal")
    except qpr.Refuse:
        print(f"ok   {name} refused")
    except Exception as exc:
        fails.append(f"{name}: {type(exc).__name__} not Refuse ({exc})")


# --- check_strides -------------------------------------------------------
SHAPE = (1, 1, 2, 4)
STRIDES = [16, 16, 8, 2]                      # contiguous rows: (h-1)*8 + 2*4 = 16 = tile
qpr.check_strides(SHAPE, STRIDES, 16)
print("ok   contiguous strides accepted")
refuses("odd element stride", lambda: qpr.check_strides(SHAPE, [16, 16, 8, 4], 32))
refuses("row too short", lambda: qpr.check_strides(SHAPE, [16, 16, 6, 2], 32))
refuses("plane too short", lambda: qpr.check_strides(SHAPE, [16, 15, 8, 2], 32))
refuses("batch too short", lambda: qpr.check_strides((2, 1, 2, 4), [15, 16, 8, 2], 64))
refuses("past buffer", lambda: qpr.check_strides(SHAPE, STRIDES, 8))

# --- surface_view / pack_surface / unpack_surface ------------------------
buf = np.zeros(32, dtype=np.float16)
view = qpr.surface_view(buf, SHAPE, STRIDES)
arr = np.arange(8, dtype=np.float16).reshape(SHAPE)
view[...] = arr
check("strided write lands", buf[:8].reshape(SHAPE), arr)

packed = qpr.pack_surface(arr, STRIDES, 64)
check("pack tile size", packed.nbytes, 64)
check("pack rows land", packed[:8].reshape(SHAPE), arr)
check("pack padding zero", packed[8:], np.zeros(24, np.float16))
check("unpack round-trip", qpr.unpack_surface(packed.tobytes(), SHAPE, STRIDES), arr)

# --- state_runs ----------------------------------------------------------
n, off = qpr.state_runs(SHAPE, STRIDES, 16)
check("contiguous single run", (n, list(off)), (16, [0]))   # two contiguous 8 B rows = one 16 B run
n, off = qpr.state_runs(SHAPE, [32, 32, 16, 2], 64)
check("split runs", (n, list(off)), (8, [0, 16]))
refuses("runs past tile", lambda: qpr.state_runs(SHAPE, [64, 64, 32, 2], 16))

# --- load_input ----------------------------------------------------------
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    np.save(tmp / "a.npy", arr)
    check("npy input", qpr.load_input(tmp / "a.npy", SHAPE), arr)
    arr.astype(np.float16).tofile(tmp / "a.f16")
    check("raw input", qpr.load_input(tmp / "a.f16", SHAPE), arr)
    sq = np.arange(16, dtype=np.float16).reshape(1, 1, 4, 4)
    sq.tofile(tmp / "sq.f16")
    check("transpose", qpr.load_input(tmp / "sq.f16", (1, 1, 4, 4), transpose=True),
          sq.swapaxes(-1, -2))

# --- port_map_from_table -------------------------------------------------
PORT = {"name": "hid", "direction": "input", "bar_slot": 0, "channel": 3,
        "buffer_id": 3, "shape": list(SHAPE), "strides": STRIDES, "tile_bytes": 64}
OUT = dict(PORT, name="out", direction="output", bar_slot=1, channel=4, buffer_id=4)
SCR = dict(PORT, name="scratch", direction="scratch", bar_slot=3, channel=0, buffer_id=0)
TABLE = {"ports": [PORT, OUT, SCR],
         "dma_coverage": [{"bar_slot": 0, "port": "hid", "buffer_id": 3},
                          {"bar_slot": 1, "port": "out", "buffer_id": 4},
                          {"bar_slot": 3, "port": "scratch", "buffer_id": 0}]}
mapped = qpr.port_map_from_table(TABLE)
check("mapped names", sorted(mapped), ["hid", "out"])
check("scratch excluded", "scratch" in mapped, False)

refuses("empty ports", lambda: qpr.port_map_from_table({"ports": []}))
refuses("dup name", lambda: qpr.port_map_from_table(
    {"ports": [PORT, dict(PORT, bar_slot=2)], "dma_coverage": TABLE["dma_coverage"]}))
refuses("bad direction", lambda: qpr.port_map_from_table(
    {"ports": [dict(PORT, direction="sideways")], "dma_coverage": TABLE["dma_coverage"]}))
refuses("dup bar_slot", lambda: qpr.port_map_from_table(
    {"ports": [PORT, dict(OUT, bar_slot=0)], "dma_coverage": TABLE["dma_coverage"]}))
refuses("channel vs buffer_id", lambda: qpr.port_map_from_table(
    {"ports": [dict(PORT, channel=9)], "dma_coverage": TABLE["dma_coverage"]}))
refuses("missing coverage", lambda: qpr.port_map_from_table(
    {"ports": TABLE["ports"], "dma_coverage": TABLE["dma_coverage"][:1]}))
refuses("coverage wrong port", lambda: qpr.port_map_from_table(
    {"ports": TABLE["ports"],
     "dma_coverage": [dict(TABLE["dma_coverage"][0], port="other")] + TABLE["dma_coverage"][1:]}))
refuses("dup coverage slot", lambda: qpr.port_map_from_table(
    {"ports": TABLE["ports"], "dma_coverage": TABLE["dma_coverage"] + [TABLE["dma_coverage"][0]]}))

# --- ane_call dry --------------------------------------------------------
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    ports = {"hid": dict(PORT), "out": dict(OUT)}
    code, cmd, outs = qpr.ane_call(tmp / "x.anec", tmp / "ports.json", ports,
                                   {"hid": arr}, tmp, "ane-run", 30, dry=True)
    check("dry rc", code, 0)
    check("dry outputs", outs, {})
    check("dry packs input", (tmp / "in-hid.surface").stat().st_size, 64)
    check("dry surface bytes",
          np.fromfile(tmp / "in-hid.surface", dtype=np.float16)[:8].reshape(SHAPE), arr)
    check("dry runs nothing", (tmp / "out-out.surface").exists(), False)
    check("dry command has lock+repeat",
          all(s in cmd for s in ("--anec", "--repeat 1", "--time")), True)

if fails:
    print("---")
    for fail in fails:
        print(f"FAIL {fail}")
    print(f"test_qwen_prog_run: {len(fails)} failed")
    sys.exit(1)
print("test_qwen_prog_run: ok")
sys.exit(0)
