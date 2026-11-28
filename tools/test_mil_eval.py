#!/usr/bin/env python3
"""Self-test for tools/mil_eval.py: MIL literal parsing and float64 eval.

Builds a small staged-style program plus a weights.bin in a temp dir and
checks parse_mil/run_mil against hand-computed numpy results. Covers the
invariants the tool relies on: BLOBFILE data starts 64 bytes after the
recorded offset, inline typed literals (bool/int32/uint64/string) and
tensor<dtype,[dims]>([...]) constants parse, every op in run_op evaluates
in float64, and the post hook sees which op arguments were constants.
"""
import importlib.util
import sys
import tempfile
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("mil_eval", REPO / "tools/mil_eval.py")
mil_eval = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mil_eval)


def write_program(tmp):
    """A program exercising const kinds, unary/binary ops, matmul with
    transpose flags, reshape, transpose, reduce, slice_by_size, concat,
    softmax and the BLOBFILE offset+64 convention."""
    blob = np.array([1.0, 2.0, 3.0, 4.0], dtype=np.float16)
    (tmp / "weights.bin").write_bytes(bytes(64) + blob.tobytes())
    (tmp / "model.mil").write_text("""program(1.3)
{
    func main<ios18>(tensor<fp16, [2, 2]> a, tensor<fp16, [2, 2]> b) {
        tensor<fp16, [2, 2]> w = const()[name = string("w"), val = tensor<fp16, [2, 2]>(BLOBFILE(path = string("@model_path/weights.bin"), offset = uint64(0)))];
        tensor<int32, [2]> p = const()[name = string("p"), val = tensor<int32, [2]>([1, 0])];
        tensor<int32, [1]> ax = const()[name = string("ax"), val = tensor<int32, [1]>([1])];
        tensor<int32, [2]> bg = const()[name = string("bg"), val = tensor<int32, [2]>([0, 0])];
        tensor<int32, [2]> sz = const()[name = string("sz"), val = tensor<int32, [2]>([1, -1])];
        tensor<int32, [2]> sh = const()[name = string("sh"), val = tensor<int32, [2]>([4, 1])];
        tensor<fp16, [2, 2]> m = mul(x = a, y = w)[name = string("m")];
        tensor<fp16, [2, 2]> mm = matmul(transpose_x = bool(false), transpose_y = bool(true), x = m, y = b)[name = string("mm")];
        tensor<fp16, [2, 2]> tr = transpose(perm = p, x = mm)[name = string("tr")];
        tensor<fp16, [1, 2]> sl = slice_by_size(begin = bg, size = sz, x = tr)[name = string("sl")];
        tensor<fp16, [2, 2]> cc = concat(axis = int32(0), interleave = bool(false), values = (sl, sl))[name = string("cc")];
        tensor<fp16, [4, 1]> rsh = reshape(shape = sh, x = cc)[name = string("rsh")];
        tensor<fp16, [1, 2]> sm = softmax(axis = ax, x = sl)[name = string("sm")];
        tensor<fp16, [1]> rs = reduce_sum(axes = ax, keep_dims = bool(false), x = sm)[name = string("rs")];
        tensor<fp16, [1, 1]> rl = reduce_l2_norm(axes = ax, keep_dims = bool(true), x = sm)[name = string("rl")];
        tensor<fp16, [2, 2]> act = silu(x = m)[name = string("act")];
    } -> (rsh, sm, rs, rl, act);
}
""")
    return (tmp / "model.mil"), blob.astype(np.float64).reshape(2, 2)


def main():
    fails = []

    def check(name, got, want):
        ok = np.allclose(got, want) if isinstance(want, np.ndarray) else got == want
        if not ok:
            fails.append(f"{name}: wanted {want}, got {got}")
            return
        print(f"ok   {name}")

    with tempfile.TemporaryDirectory() as d:
        mil_path, w = write_program(Path(d))
        program = mil_eval.parse_mil(mil_path)
        shapes, consts, ops, returns = program
        check("shapes", shapes, {"a": [2, 2], "b": [2, 2]})
        check("returns", returns, ["rsh", "sm", "rs", "rl", "act"])
        check("blob const (+64)", consts["w"], w)

        a = np.array([[0.5, -1.0], [2.0, 0.25]])
        b = np.array([[1.0, 3.0], [-2.0, 0.5]])
        out = mil_eval.run_mil(program, {"a": a, "b": b})

        m = a * w
        mm = m @ b.T
        tr = mm.T
        sl = tr[0:1, :]
        cc = np.concatenate([sl, sl], axis=0)
        sm_in = sl - sl.max(axis=1, keepdims=True)
        sm = np.exp(sm_in) / np.exp(sm_in).sum(axis=1, keepdims=True)
        check("mul/matmulT/transpose/slice/concat/reshape",
              out["rsh"], cc.reshape(4, 1))
        check("softmax", out["sm"], sm)
        check("reduce_sum", out["rs"], sm.sum(axis=1))
        check("reduce_l2_norm", out["rl"], np.sqrt((sm ** 2).sum(axis=1, keepdims=True)))
        check("silu", out["act"], m / (1 + np.exp(-m)))

        # post hook: sees const argument names and replaces op results
        seen = {}

        def post(op, value, const_args):
            seen[op] = const_args
            return np.where(op == "mul", np.round(value), value)

        posted = mil_eval.run_mil(program, {"a": a, "b": b}, post=post)
        check("post rounds mul", posted["act"], np.round(m) / (1 + np.exp(-np.round(m))))
        check("post sees const args", "y" in seen["mul"] and "perm" in seen["transpose"], True)
        check("post sees no const args on inputs", seen["concat"] & {"x"}, set())

        # BLOBFILE of a non-fp16 dtype is refused
        bad = Path(d) / "bad.mil"
        bad.write_text((mil_path.read_text().replace(
            'val = tensor<fp16, [2, 2]>(BLOBFILE',
            'val = tensor<int32, [2, 2]>(BLOBFILE')))
        try:
            mil_eval.parse_mil(bad)
            fails.append("non-fp16 BLOBFILE: no error")
        except ValueError:
            print("ok   non-fp16 BLOBFILE refused")

        # an op outside run_op's table raises ValueError
        try:
            mil_eval.run_op("gelu", {"x": np.zeros(1)})
            fails.append("unknown op: no error")
        except ValueError:
            print("ok   unknown op refused")

        # missing weights.bin fails at parse (memmap), not mid-eval
        (Path(d) / "weights.bin").unlink()
        try:
            mil_eval.parse_mil(mil_path)
            fails.append("missing weights.bin: no error")
        except OSError:
            print("ok   missing weights.bin refused")

    if fails:
        print("---")
        for fail in fails:
            print(f"FAIL {fail}")
        print(f"test_mil_eval: {len(fails)} failed")
        return 1
    print("test_mil_eval: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
