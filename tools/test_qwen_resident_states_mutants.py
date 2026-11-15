#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Mutation proof for test_qwen_resident_states.py (host only). Each mutant is a copy of qwen_prog_run.py or
qwen_m2_decode.py with one change, run with the real test and the real (unmutated) fake-device session. The control
must pass; every mutant must fail (nonzero exit, a FAIL line, or a hang killed by the timeout)."""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
REPO = TOOLS.parent
FILES = ("qwen_prog_run.py", "qwen_m2_decode.py", "qwen_m2_conform.py", "pcore_mask.py", "test_qwen_resident_states.py")

M = {
    "qwen_prog_run.py": {
        "whole_tile_run": ("    run = 2 * w\n    if len(rows) and", "    run = 2 * w\n    return tile_bytes, rows[:1].astype(np.uint64)\n    if len(rows) and"),
        "offsets_from_wrong_stride": ("np.arange(h, dtype=np.int64)[None, None, :] * strides[2]", "np.arange(h, dtype=np.int64)[None, None, :] * (2 * w)"),
        "call_sends_resident_inputs": ('p["direction"] == "input" and n not in res["in"]]', 'p["direction"] == "input"]'),
        "call_reads_resident_outputs": ('p["direction"] == "output" and n not in res["out"]]', 'p["direction"] == "output"]'),
        "pair_output_by_shape": ("{outs.index(out_port)}", "{outs.index([n for n in outs if ports[n]['shape'] == a['shape']][0])}"),
        "no_pair_equality_check": ("                if a[key] != b[key]:", "                if False:"),
        "pair_check_after_send": (
            '            for key in ("shape", "strides", "surface_bytes", "tile_bytes"):\n                if a[key] != b[key]:\n                    raise Refuse(f"{name}: state pair {in_port}->{out_port} differs in {key}: {a[key]} vs {b[key]}")\n', ""),
        "no_dtype_check": ('                if spec.get("dtype") not in ("fp16", "float16", "f16"):', "                if False:"),
        "reset_states_noop": ('        self._command("RESET")\n        if self._line() != "OK RESET":\n            self._fail("RESET", "", "bad reply")', "        pass"),
    },
    "qwen_m2_decode.py": {
        "reset_not_sent": ("            self.session.reset_states()\n", "            pass\n"),
        "states_not_registered": ("                if self.resident_states:\n                    self.session.states(", "                if False:\n                    self.session.states("),
        "host_still_chains": ("            if not resident:\n                for s in pr[\"states\"]:", "            if True:\n                for s in pr[\"states\"]:"),
        "decoder_dump_refusal_removed": ('        if self.resident_states and (not getattr(args, "resident", False) or getattr(args, "dump", None)):', "        if False:"),
        "state_in_still_in_arrays": ("for s in pr[\"srcs\"] if not (resident and s[\"kind\"] == \"state_in\")}", "for s in pr[\"srcs\"]}"),
    },
}


def run(work, serve):
    env = dict(os.environ, ANE_REPO=str(REPO), ANE_SESSION_STATE_SERVE=str(serve), PYTHONDONTWRITEBYTECODE="1")
    try:
        p = subprocess.run([sys.executable, str(work / "test_qwen_resident_states.py")], capture_output=True, text=True,
                           timeout=90, env=env, cwd=work, check=False)
    except subprocess.TimeoutExpired:
        return "HANG"
    return "PASS" if p.returncode == 0 and "FAIL" not in p.stdout else "FAIL"


def main():
    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        libane, uapi = REPO / "libane", REPO / "ane/src/uapi/drm"
        serve = root / "serve"
        subprocess.run(["gcc", "-O1", "-std=gnu99", "-w", f"-I{libane}", f"-I{uapi}", "-I/usr/include/libdrm",
                        "-Wl,--wrap=open,--wrap=ioctl,--wrap=mmap", "-o", str(serve), str(TOOLS / "test_ane_session_state.c"),
                        str(libane / "ane.c"), str(libane / "ane_m2.c"), str(libane / "ane_sha256.c"), "-lm"],
                       check=True, cwd=TOOLS)
        work = root / "control"
        work.mkdir()
        for f in FILES:
            shutil.copy(TOOLS / f, work / f)
        control = run(work, serve)
        print(f"control (unmutated): {control}")
        if control != "PASS":
            return 1
        bad = 0
        total = 0
        for fname, muts in M.items():
            src = (TOOLS / fname).read_text()
            for name, (old, new) in muts.items():
                total += 1
                if src.count(old) != 1:
                    print(f"MUTANT {name}: pattern found {src.count(old)} times in {fname}")
                    bad += 1
                    continue
                w = root / name
                w.mkdir()
                for f in FILES:
                    shutil.copy(TOOLS / f, w / f)
                (w / fname).write_text(src.replace(old, new))
                res = run(w, serve)
                print(f"mutant {name}: {res}{'  (killed)' if res in ('FAIL', 'HANG') else '  SURVIVED'}")
                bad += res not in ("FAIL", "HANG")
        print(f"{total - bad} of {total} mutants killed")
        return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
