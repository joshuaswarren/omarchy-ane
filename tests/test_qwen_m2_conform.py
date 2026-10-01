#!/usr/bin/env python3
"""Host checks for the M2 conformance harness: metrics, output matching, the
binding decision, and packing of a real dumped program input. No device."""

import hashlib
import sys
import tempfile
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import numpy as np

from qwen_m2_conform import compare, decide, evaluate, match_outputs, resolved_table, summarize
from qwen_prog_run import ane_call, port_map_from_table, unpack_surface

FIX = ROOT / "tests/fixtures"
# Step 11 inputs of prog_000 from the M1 per-step dump (qwen38-step-goldens,
# index.json sha256 of each port file).
PROG000_STEP11 = {
    "t1": "51b3ca0bd59a350ce2f511bd222aa0798f25444fc6f15384a1813fb57d3f56cb",
    "t5": "3b52aa0a99d8e90671d5cf70e680b5010ba4181a6ba26bed38def13e5089746c",
}


def test_compare_metrics():
    ref = np.array([1, 2, 3, 4], dtype=np.float16)
    assert compare(ref, ref) == {"rel_l2": 0.0, "max_abs": 0.0, "exact": 1.0, "nonfinite": 0}
    dev = ref.copy()
    dev[3] = 5
    got = compare(dev, ref)
    assert got["max_abs"] == 1.0 and got["exact"] == 0.75
    assert abs(got["rel_l2"] - 1 / np.sqrt(30)) < 1e-12
    dev[0] = np.nan
    assert compare(dev, ref)["nonfinite"] == 1 and compare(dev, ref)["rel_l2"] == float("inf")
    zero = np.zeros(4, dtype=np.float16)
    assert compare(zero, zero)["rel_l2"] == 0.0
    assert compare(ref, zero)["rel_l2"] == float("inf")


def test_match_outputs_follows_the_data_not_the_names():
    rng = np.random.default_rng(0)
    a, b, c = (rng.standard_normal(256).astype(np.float16) for _ in range(3))
    meta = {n: {"threshold": 0.02} for n in "abc"}
    dev = {"a": b, "b": a, "c": c}
    read, passing = match_outputs(dev, {"a": a, "b": b, "c": c}, meta, [["a", "b"]])
    assert read == {"a": "b", "b": "a", "c": "c"} and passing == [1]
    # Equal M1 outputs cannot tell the slots apart: both assignments pass.
    _, passing = match_outputs({"a": a, "b": a}, {"a": a, "b": a}, meta, [["a", "b"]])
    assert passing == [2]


def test_summary_ranks_ports_by_their_own_threshold():
    ref = {"x": np.ones(64, np.float16), "k": np.ones(64, np.float16)}
    dev = {"x": ref["x"] * np.float16(1.03), "k": ref["k"] * np.float16(1.1)}
    meta = {"x": {"threshold": 0.02}, "k": {"threshold": 0.18}}
    s = summarize(evaluate(dev, ref, meta, {"x": "x", "k": "k"}))
    assert s["worst_port"] == "x" and not s["pass"] and s["worst_ratio"] > 1


def trial(feed, ok, worst, read="identity", out="1"):
    return feed, {"pass": ok, "worst_ratio": worst, "read": read, "out_group_passing": out}


def test_decide_binding():
    ident = {"x": "x", "y": "y", "s": "s"}
    swap = {"x": "y", "y": "x", "s": "s"}
    groups, out_groups = [["x", "y"]], [["o", "p"]]
    feed, read, ins, outs = decide(groups, [True], out_groups, [
        trial(ident, False, 0.31), trial(swap, True, 0.0012, read="o=p;p=o")])
    assert feed == swap and read == {"o": "p", "p": "o"} and ins == ["decided"] and outs == ["decided"]
    # A graph symmetric in x and y passes both ways.
    _, _, ins, _ = decide(groups, [True], out_groups, [trial(ident, True, 0.001), trial(swap, True, 0.002)])
    assert ins == ["undecided"]
    _, _, ins, outs = decide(groups, [True], out_groups, [trial(ident, False, 0.4, out="0"),
                                                           trial(swap, False, 0.3, out="0")])
    assert ins == ["fail"] and outs == ["fail"]
    _, _, ins, outs = decide(groups, [False], out_groups, [trial(ident, True, 0.001, out="2")])
    assert ins == ["identical-inputs"] and outs == ["undecided"]


def test_real_prog000_input_packs_to_the_port_table():
    table = json.loads((FIX / "prog000-ports.json").read_text())
    ports = port_map_from_table(table)
    arrays = {}
    for name, digest in PROG000_STEP11.items():
        raw = (FIX / f"prog000-step11-in_{name}.f16").read_bytes()
        assert hashlib.sha256(raw).hexdigest() == digest
        arrays[name] = np.frombuffer(raw, dtype=np.float16)
    with tempfile.TemporaryDirectory() as tmp:
        status, command, outputs = ane_call("/nonexistent.anec", FIX / "prog000-ports.json", ports,
                                            arrays, Path(tmp), "ane-run", 120, dry=True)
        assert status == 0 and outputs == {}
        assert command.startswith("flock /var/tmp/ane-run.lock timeout 120 ane-run ")
        for name, port in ports.items():
            flag = "--in" if port["direction"] == "input" else "--out"
            assert f"{flag} {name}={tmp}/" in command
        for name in arrays:
            surface = (Path(tmp) / f"in-{name}.surface").read_bytes()
            assert len(surface) == ports[name]["tile_bytes"]
            unpacked = unpack_surface(surface, ports[name]["shape"], ports[name]["strides"])
            assert np.array_equal(unpacked.ravel(), arrays[name])
            assert np.count_nonzero(np.frombuffer(surface, np.float16)) == np.count_nonzero(arrays[name])
    assert [ports[n]["tile_bytes"] for n in ("t1", "t5")] == [16384, 393216]


def test_resolved_table_moves_names_between_slots():
    table = json.loads((FIX / "prog000-ports.json").read_text())
    names = {"t16": "t20", "t20": "t16", "t26": "t35", "t35": "t26"}
    out = resolved_table(table, names, ["a", "b"])
    slot = {p["name"]: p["bar_slot"] for p in out["ports"]}
    before = {p["name"]: p["bar_slot"] for p in table["ports"]}
    assert slot["t16"] == before["t20"] and slot["t35"] == before["t26"] and slot["t1"] == before["t1"]
    assert [a["conform"] for a in out["ambiguities"]] == ["a", "b"]
    port_map_from_table(out)


if __name__ == "__main__":
    test_compare_metrics()
    test_match_outputs_follows_the_data_not_the_names()
    test_summary_ranks_ports_by_their_own_threshold()
    test_decide_binding()
    test_real_prog000_input_packs_to_the_port_table()
    test_resolved_table_moves_names_between_slots()
    print("conformance harness checks passed")
