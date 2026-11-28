#!/usr/bin/env python3
"""Self-test for tools/mil_graph_equiv.py: parse, WL labels, lanes and links.

Feeds it small staged-style MIL programs from a temp dir. The cases cover the
invariants the tool's docstring promises: the fused graph equals the chain
when the wired-in tensor replaces the second program's argument; a swap of
two same-shaped inputs is invisible without lanes but caught with them;
BLOBFILE offsets and name= strings are masked out; a dropped output lane or
a different op fails; unparseable input exits 2.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
TOOL = REPO / "tools/mil_graph_equiv.py"

FIRST = """program(1.3)
{
    func main<ios18>(tensor<fp16, [8]> a, tensor<fp16, [8]> b) {
        tensor<fp16, [8]> t = add(x = a, y = b)[name = string("t")];
    } -> (t);
}
"""

SECOND = """program(1.3)
{
    func main<ios18>(tensor<fp16, [8]> link, tensor<fp16, [8]> c) {
        tensor<int32, [1]> k = const()[name = string("k"), val = tensor<int32, [1]>([2])];
        tensor<fp16, [8]> w = const()[name = string("w"), offset = uint64(64), val = BLOBFILE(path = string("@model_path/weights.bin"))];
        tensor<fp16, [8]> r = mul(x = link, y = c)[name = string("r")];
    } -> (r);
}
"""

# the chain of FIRST then SECOND with SECOND's `link` wired to FIRST's `t`
FUSED = """program(1.3)
{
    func main<ios18>(tensor<fp16, [8]> q, tensor<fp16, [8]> s, tensor<fp16, [8]> c) {
        tensor<fp16, [8]> t = add(x = q, y = s)[name = string("t")];
        tensor<int32, [1]> k = const()[name = string("k"), val = tensor<int32, [1]>([2])];
        tensor<fp16, [8]> w = const()[name = string("w"), offset = uint64(128), val = BLOBFILE(path = string("@model_path/weights.bin"))];
        tensor<fp16, [8]> r = mul(x = t, y = c)[name = string("other")];
    } -> (r);
}
"""

# same fused graph but the second program's add operands swapped roles:
# FIRST's b is fed by lane s, yet here lane q feeds y and lane s feeds x.
FUSED_SWAPPED = FUSED.replace("add(x = q, y = s)", "add(x = s, y = q)")

FUSED_SUB = FUSED.replace("r = mul(x = t, y = c)", "r = sub(x = t, y = c)")

FUSED_EXTRA_OUT = FUSED.replace("} -> (r);", "} -> (r, t);")


OUT_LANES = ["--out-fused", "r=hand", "--out-second", "r=hand"]


def run(tmp, fused, first=FIRST, second=SECOND, extra=None):
    paths = {}
    for name, text in (("fused", fused), ("first", first), ("second", second)):
        paths[name] = Path(tmp) / f"{name}.mil"
        paths[name].write_text(text)
    cmd = [sys.executable, str(TOOL), str(paths["fused"]), str(paths["first"]),
           str(paths["second"]), "--link", "link=t"] + list(extra or [])
    return subprocess.run(cmd, capture_output=True, text=True)


def main():
    fails = []

    def check(name, got, want):
        if got != want:
            fails.append(f"{name}: wanted {want}, got {got}")
            return
        print(f"ok   {name}")

    with tempfile.TemporaryDirectory() as tmp:
        # a real chain fold: equal, exit 0. Output lanes must be named for
        # the comparison to see any outputs at all.
        result = run(tmp, FUSED, extra=OUT_LANES)
        check("chain equal rc", result.returncode, 0)
        check("chain equal verdict", result.stdout.splitlines()[-1], "EQUAL")

        # offsets and name= strings are masked: equal holds even though the
        # fused weight offset is 128 and the second program's is 64
        check("offset masked", result.returncode, 0)

        # no --out lanes: label multisets still equal, but no output lane is
        # compared so the verdict cannot be equal
        result = run(tmp, FUSED)
        check("no out lanes rc", result.returncode, 1)

        # a different op in the fused program
        check("different op rc", run(tmp, FUSED_SUB, extra=OUT_LANES).returncode, 1)

        # a fused output that re-exports the linked intermediate under a new
        # lane name: the chain has no such lane, so not equal
        result = run(tmp, FUSED_EXTRA_OUT, extra=["--out-fused", "r=hand,t=first_out",
                                                  "--out-second", "r=hand",
                                                  "--out-first", "t=first_out"])
        check("re-exported intermediate rc", result.returncode, 1)

        # same-shaped input swap: invisible without lanes, caught with them
        result = run(tmp, FUSED_SWAPPED, extra=OUT_LANES)
        check("swap unlabelled rc", result.returncode, 0)
        lanes = ["--lanes-fused", "q=a_lane,s=b_lane,c=c_lane",
                 "--lanes-first", "a=a_lane,b=b_lane",
                 "--lanes-second", "link=x,c=c_lane"]
        result = run(tmp, FUSED_SWAPPED, extra=OUT_LANES + lanes)
        check("swap labelled rc", result.returncode, 1)

        # unparseable input
        bad = Path(tmp) / "bad.mil"
        bad.write_text("not mil at all\n")
        result = subprocess.run([sys.executable, str(TOOL), str(bad),
                                 str(Path(tmp) / "first.mil"), str(Path(tmp) / "second.mil"),
                                 "--link", "link=t"], capture_output=True, text=True)
        check("parse error rc", result.returncode, 2)

    if fails:
        print("---")
        for fail in fails:
            print(f"FAIL {fail}")
        print(f"test_mil_graph_equiv: {len(fails)} failed")
        return 1
    print("test_mil_graph_equiv: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
