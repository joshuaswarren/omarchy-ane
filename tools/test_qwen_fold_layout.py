#!/usr/bin/env python3
"""Self-test for tools/qwen_fold_layout.py: folded-layout manifest builder.

Stages a control manifest (a state block + its consumer + one plain
program), the control compile record (PORTMAP lines + MIL files) and a
layout record whose first program is the chain of the first two control
programs. Pins: the fold detection (state block followed by a program,
MIL hash differs), the copied-entry path (same MIL hash + same PORTMAP
entry), the lane-shape checks, the mil_graph_equiv chain check, --no-equiv,
--self-check, and every refusal.
"""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
TOOL = REPO / "tools/qwen_fold_layout.py"


def mil(args, ops, outs):
    body = "\n".join(
        f"        tensor<fp16, [4]> {n} = {op}({', '.join(f'{k} = {v}' for k, v in a)})[];"
        for n, op, a in ops)
    sig = ", ".join(f"tensor<fp16, [4]> {a}" for a in args)
    return f"program(1.3)\n{{\n    func main<ios18>({sig}) {{\n{body}\n    }} -> ({', '.join(outs)});\n}}\n"


# control prog0: the state block (inputs beta gt k q v, output o)
MIL0 = mil(["beta", "gt", "k", "q", "v"],
           [("m1", "add", (("x", "beta"), ("y", "gt"))),
            ("m2", "add", (("x", "k"), ("y", "q"))),
            ("s", "mul", (("x", "m1"), ("y", "v"))),
            ("o", "mul", (("x", "s"), ("y", "m2")))],
           ["o"])
# control prog1: consumes o and a, outputs res
MIL1 = mil(["o", "a"],
           [("r1", "add", (("x", "o"), ("y", "a"))),
            ("res", "mul", (("x", "r1"), ("y", "o")))],
           ["res"])
# control prog2: a plain program
MIL2 = mil(["z"], [("w", "add", (("x", "z"), ("y", "z")))], ["w"])
# the layout's folded program 0: prog0 then prog1 with o wired inside
MIL_FUSED = mil(["beta", "gt", "k", "q", "v", "a"],
                [("m1", "add", (("x", "beta"), ("y", "gt"))),
                 ("m2", "add", (("x", "k"), ("y", "q"))),
                 ("s", "mul", (("x", "m1"), ("y", "v"))),
                 ("o", "mul", (("x", "s"), ("y", "m2"))),
                 ("r1", "add", (("x", "o"), ("y", "a"))),
                 ("res", "mul", (("x", "r1"), ("y", "o")))],
                ["res"])
# a stale fold: right ports, wrong graph (r1 dropped)
MIL_STALE = mil(["beta", "gt", "k", "q", "v", "a"],
                [("m1", "add", (("x", "beta"), ("y", "gt"))),
                 ("m2", "add", (("x", "k"), ("y", "q"))),
                 ("s", "mul", (("x", "m1"), ("y", "v"))),
                 ("o", "mul", (("x", "s"), ("y", "m2"))),
                 ("res", "mul", (("x", "o"), ("y", "a")))],
                ["res"])

PM0 = {"x": {"beta": "beta", "gt": "gt", "k": "k", "q": "q", "v": "v"}, "h": {"o": "o"}}
PM1 = {"x": {"o": "o", "a": "a"}, "h": {"res": "res"}}
PM2 = {"x": {"z": "z"}, "h": {"w": "w"}}
PM_FUSED = {"x": {"beta": "beta", "gt": "gt", "k": "k", "q": "q", "v": "v", "a": "a"}, "h": {"res": "res"}}


def entry(srcs, dsts):
    return {"srcs": [{"port": l, "kind": "lane", "lane": l, "shape": [4]} for l in srcs],
            "dsts": [{"port": l, "kind": "lane", "lane": l, "shape": [4]} for l in dsts],
            "states": [], "group_start": False, "group_end": False}


CONTROL = {"n_programs": 3, "programs": [
    entry(["beta", "gt", "k", "q", "v"], ["o"]),
    entry(["o", "a"], ["res"]),
    entry(["z"], ["w"]),
]}


def record_dir(root, name, programs):
    """programs: [(index, hash_name, portmap, mil_text)]"""
    d = root / name
    (d / "mil").mkdir(parents=True)
    lines = []
    for index, h, pm, text in programs:
        (d / "mil" / f"{index:02d}-{h}.mil").write_text(text)
        lines.append(f"PORTMAP {index} {json.dumps(pm)}")
    (d / "probe.out").write_text("\n".join(lines) + "\n")
    return d


def stage(tmp, layout_mils, layout_pms, name="case"):
    root = Path(tmp) / name
    (root / "record").mkdir(parents=True)
    record_dir(root / "record", "control", [(0, "c0", PM0, MIL0), (1, "c1", PM1, MIL1), (2, "c2", PM2, MIL2)])
    record_dir(root / "record", "foldx", [(i, h, pm, text)
                                          for i, (h, text, pm) in enumerate(zip(*layout_mils, layout_pms))])
    (root / "control.json").write_text(json.dumps(CONTROL))
    return root


GOOD_LAYOUT = (["f0", "c2"], [MIL_FUSED, MIL2])
GOOD_PMS = [PM_FUSED, PM2]


def run(root, layout="foldx", extra=()):
    return subprocess.run([sys.executable, str(TOOL), str(root / "control.json"),
                           str(root / "record"), layout, str(root / "out.json"), *extra],
                          capture_output=True, text=True)


def main():
    fails = []

    def check(name, got, want):
        if got != want:
            fails.append(f"{name}: wanted {want}, got {got}")
            return
        print(f"ok   {name}")

    with tempfile.TemporaryDirectory() as tmp:
        root = stage(tmp, GOOD_LAYOUT, GOOD_PMS)

        result = run(root)
        check("fold rc", result.returncode, 0)
        manifest = json.loads((root / "out.json").read_text())
        check("mapping", manifest["fold_mapping"], [[0, 1], [2]])
        check("n_programs", manifest["n_programs"], 2)
        check("equiv flagged", manifest["equiv_checked"], True)
        check("folded srcs lanes", sorted(s["lane"] for s in manifest["programs"][0]["srcs"]),
              ["a", "beta", "gt", "k", "q", "v"])
        check("copied entry", manifest["programs"][1], CONTROL["programs"][2])

        # a stale fold (right ports, wrong graph) is refused by the chain check
        root = stage(tmp, (["f0", "c2"], [MIL_STALE, MIL2]), GOOD_PMS, name="stale")
        result = run(root)
        check("stale rc", result.returncode, 1)
        check("stale reason", "not the chain" in result.stdout, True)

        # --no-equiv lets the same stale fold through and flags the manifest
        result = run(root, extra=("--no-equiv",))
        check("no-equiv rc", result.returncode, 0)
        manifest = json.loads((root / "out.json").read_text())
        check("no-equiv flagged", manifest["equiv_checked"], False)

        # a PORTMAP entry that does not match the control manifest
        root = stage(tmp, (["f0", "c2"], [MIL_FUSED, MIL2]),
                     [PM_FUSED, dict(PM2, group_start=True)], name="mismatch")
        result = run(root)
        check("entry mismatch rc", result.returncode, 1)
        check("entry mismatch", "PORTMAP entry differs" in result.stdout, True)

        # --self-check rebuilds every control entry from its record
        root = stage(tmp, GOOD_LAYOUT, GOOD_PMS, name="selfcheck")
        result = run(root, extra=("--self-check",))
        check("self-check rc", result.returncode, 0)
        check("self-check line", "all 3 control entries rebuilt equal" in result.stdout, True)

        # usage problem
        result = subprocess.run([sys.executable, str(TOOL), "one.json"],
                                capture_output=True, text=True)
        check("usage rc", result.returncode, 2)

    if fails:
        print("---")
        for fail in fails:
            print(f"FAIL {fail}")
        print(f"test_qwen_fold_layout: {len(fails)} failed")
        return 1
    print("test_qwen_fold_layout: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
