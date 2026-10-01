#!/usr/bin/env python3
"""Host checks for the M2 staged-Qwen decode: context tables bit for bit
against the M1 step dump (and, per prompt max_len M, the dumped tables cut to
M), per-M program and state selection, lane and state chaining across
programs and steps, and the token comparison helpers. No device."""

import argparse
import hashlib
import json
import sys
import tempfile
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from qwen_m2_decode import Decoder, ctx_vals, first_divergence, rope_tables, top2
from qwen_prog_run import Refuse

# sha256 of the context inputs of prog_006 in the M1 per-step dump of p001
# (qwen38-step-goldens index.json), steps 0 and 11.
DUMP_CTX = {
    0: {"oh": "6f8f8ff62d8952bc25932e3a64b720363c6224e6d7ab4f0d7c2d2eec908d187d",
        "inv": "c7ac45098b601a3586e031e235b0829ef2ccdf2d846dac5b588c1bfcea9361bc",
        "mask": "f33a3c2464dd97bded31cc6f9e86d3e1af354d0b00ef47a8186b0e7fa392dd1c",
        "cosp": "b52f1a391580ca061d258dac1af97a7a9c494c68059238948fc2f98f3d4e5028",
        "sinp": "076a27c79e5ace2a3d47f9dd2e83e4ff6ea8872b3c2218f66c92b89b55f36560"},
    11: {"oh": "60fb23f8fdc91870067e8f5d3b25e5d429bab8a2cd1f30dc4f99e2dfd5293ab5",
         "inv": "be046b741686114ba13572177ab22e92c5d37ea7ab8fba394ca4688e43f58c5e",
         "mask": "e0e7cb61df384321896fb9942e73c2d225a6e0c37ba30877be0020058cc50dcf",
         "cosp": "ea09974ac0d515c3846786569d26220fcada7f3bc7a36cb85c6a624d9c56c189",
         "sinp": "a3883a0f8ceed9646229e2d051dcdf970d278b6b6d575942a306f0e97ad3edd4"},
}


def test_context_tables_match_the_m1_dump():
    # Qwen3.8-2B GGUF metadata: key_length 256, rope dimension_count 64, freq_base 1e7.
    cos, sin = rope_tables(50, 256, 64, 10_000_000.0)
    for pos, want in DUMP_CTX.items():
        got = {k: hashlib.sha256(np.ascontiguousarray(v).tobytes()).hexdigest()
               for k, v in ctx_vals(pos, 50, cos, sin).items()}
        assert got == want, pos


def test_context_tables_at_m_are_the_dumped_m50_tables_cut_to_m():
    # ANEForge rebuilds the rope table at M = len(prompt) + 32 (43..50 for chunk_00).
    cos50, sin50 = rope_tables(50, 256, 64, 10_000_000.0)
    for m in range(43, 51):
        cos, sin = rope_tables(m, 256, 64, 10_000_000.0)
        assert cos.tobytes() == cos50[:m].tobytes() and sin.tobytes() == sin50[:m].tobytes(), m
        for pos in (0, 11, m - 2):
            ref = ctx_vals(pos, 50, cos50, sin50)
            cut = {"oh": ref["oh"][:, :m], "inv": ref["inv"][:, :m], "mask": ref["mask"][..., :m],
                   "cosp": ref["cosp"], "sinp": ref["sinp"]}
            for key, got in ctx_vals(pos, m, cos, sin).items():
                want = np.ascontiguousarray(cut[key])
                assert got.shape == want.shape and got.tobytes() == want.tobytes(), (m, pos, key)


def _table(prog, ports):
    rows = [{"name": n, "direction": d, "shape": s, "bar_slot": 4 + k, "buffer_id": 4 + k, "channel": 4 + k}
            for k, (n, d, s) in enumerate(ports)]
    return {"program": prog, "ports": rows, "exceptions": [],
            "dma_coverage": [{"bar_slot": r["bar_slot"], "port": r["name"], "buffer_id": r["buffer_id"]}
                             for r in rows]}


def test_per_m_programs_and_states_follow_the_prompt_max_len():
    manifest = {"max_len": 4, "programs": [
        {"srcs": [{"port": "oh", "kind": "ctx", "lane": "oh", "shape": [1, 4, 1]},
                  {"port": "s", "kind": "state_in", "lane": "state0", "shape": [2, 4, 3]}],
         "dsts": [{"port": "y", "kind": "lane", "lane": "y", "shape": [1, 2]}],
         "states": [{"in_port": "s", "in_shape": [2, 4, 3], "out_port": "s2", "out_shape": [2, 4, 3]}]},
        {"srcs": [{"port": "a", "kind": "lane", "lane": "y", "shape": [1, 2]}],
         "dsts": [{"port": "h", "kind": "lane", "lane": "h", "shape": [1, 2]}], "states": []}]}
    attention = lambda m: [("oh", "input", [1, 1, m, 1]), ("s", "input", [1, 2, m, 3]),
                           ("y", "output", [1, 1, 1, 2]), ("s2", "output", [1, 2, m, 3])]
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        files = {"base/prog_000": _table("prog_000", attention(4)),
                 "base/prog_001": _table("prog_001", [("a", "input", [1, 2]), ("h", "output", [1, 2])]),
                 "perm/M3/prog_000": _table("prog_000", attention(3)),
                 "perm/M2/prog_000": _table("prog_000", attention(3))}
        for rel, table in files.items():
            (tmp / rel).mkdir(parents=True)
            (tmp / rel / "ports.json").write_text(json.dumps(table))
        args = argparse.Namespace(out=str(tmp / "out"), anec_dir=str(tmp / "base"), ports_dir=None,
                                  per_m_dir=str(tmp / "perm"))
        dec = Decoder(args, manifest)
        assert [t[0] for t in dec.tables] == [tmp / "base/prog_000/program-0.anec",
                                              tmp / "base/prog_001/program-0.anec"]
        dec.states[0]["s"][:] = 1
        dec.configure(3)
        assert dec.max_len == 3 and dec.states[0]["s"].shape == (2, 3, 3) and not dec.states[0]["s"].any()
        assert [t[0] for t in dec.tables] == [tmp / "perm/M3/prog_000/program-0.anec",
                                              tmp / "base/prog_001/program-0.anec"]
        try:
            dec.configure(2)  # a table built for M=3 must not pass as M=2
        except Refuse:
            pass
        else:
            raise AssertionError("configure(2) accepted an M=3 port table")


def test_lanes_and_states_chain_across_programs_and_steps():
    progs = [
        {"group_start": True, "group_end": False,
         "srcs": [{"port": "a", "kind": "lane", "lane": "x"},
                  {"port": "s", "kind": "state_in", "lane": "state0"}],
         "dsts": [{"port": "b", "lane": "y"}],
         "states": [{"in_port": "s", "in_shape": [2], "out_port": "s2"}]},
        {"group_start": False, "group_end": True,
         "srcs": [{"port": "c", "kind": "lane", "lane": "y"},
                  {"port": "p", "kind": "ctx", "lane": "oh"}],
         "dsts": [{"port": "d", "lane": "h"}], "states": []},
    ]
    dec = Decoder.__new__(Decoder)
    dec.progs, dec.max_len = progs, 4
    dec.reset()

    def call(i, arrays):
        if i == 0:
            return {"b": arrays["a"].ravel() + arrays["s"], "s2": arrays["s"] + 1}, None
        return {"d": arrays["c"] * 2 + arrays["p"].ravel()[1]}, None
    dec.call = call
    embed = np.array([[1, 2], [10, 20]], np.float16)
    cos, sin = rope_tables(4, 4, 4, 10000.0)
    h, timing = dec.step(1, 0, cos, sin, embed)
    assert h.tolist() == [20, 40] and len(timing) == 2  # state 0 at the start, oh[1] = 0 at pos 0
    h, _ = dec.step(0, 1, cos, sin, embed)
    assert h.tolist() == [5, 7]  # (embed[0] + state 1) * 2 + oh[1] = 1 at pos 1
    dec.reset()
    assert dec.states[0]["s"].tolist() == [0, 0]


def test_token_comparison_helpers():
    assert first_divergence([1, 2, 3], [1, 2, 3]) is None
    assert first_divergence([1, 5, 3], [1, 2, 3]) == 1
    assert first_divergence([1, 2], [1, 2, 3]) == 2
    assert top2(np.array([3, 7, 7, 1], np.float32)) == (1, 7.0, 2, 7.0)


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
    print("ok")
