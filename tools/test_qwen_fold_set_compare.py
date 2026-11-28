#!/usr/bin/env python3
"""Self-test for tools/qwen_fold_set_compare.py: two-boot fold-set report.

Feeds it synthetic control/layout summaries plus a fold manifest and pins:
the exec-saved math (replaced sum minus folded median, noise as a linear
sum of per-program ranges), the outside_noise verdict, the 5 percent
shared-program boot-drift gate, the step-ratio screen, and every rc-2
refusal (not frozen, listed failures, layout made against another control
summary, a summary that is not a one-arm run, bad usage).
"""
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
TOOL = REPO / "tools/qwen_fold_set_compare.py"


def summary(medians, ranges=None, p50=None, spread=None, frozen=True, failures=None,
            control_sha=None, identity=None):
    out = {
        "frozen": frozen,
        "failures": failures or [],
        "timing": {
            "exec_ms_median": medians,
            "exec_ms_range": ranges or {k: [v, v] for k, v in medians.items()},
            "step_p50_s": p50 or [1.0, 1.0, 1.0],
        },
    }
    if spread is not None:
        out["timing"]["spread"] = spread
    if control_sha is not None:
        out["inputs"] = {"control_summary_sha256": control_sha}
    if identity is not None:
        out["identity"] = identity
    return out


def run(tmp, control, layout, manifest, extra_args=None):
    paths = []
    for name, obj in (("control.json", control), ("layout.json", layout), ("manifest.json", manifest)):
        p = Path(tmp) / name
        p.write_text(json.dumps(obj))
        paths.append(str(p))
    if extra_args is not None:
        paths = extra_args
    return subprocess.run([sys.executable, str(TOOL), *paths], capture_output=True, text=True)


def main():
    fails = []

    def check(name, got, want):
        if got != want:
            fails.append(f"{name}: wanted {want}, got {got}")
            return
        print(f"ok   {name}")

    with tempfile.TemporaryDirectory() as tmp:
        control = summary({"prog_000": 10.0, "prog_001": 5.0, "prog_002": 4.0, "prog_003": 1.0},
                          ranges={"prog_000": [9.5, 10.5], "prog_001": [5.0, 5.0],
                                  "prog_002": [4.0, 4.0], "prog_003": [1.0, 1.0]},
                          p50=[1.0, 1.1, 0.9], spread=0.02)
        control_sha = hashlib.sha256(json.dumps(control).encode()).hexdigest()
        manifest = {"fold_mapping": [[0, 1], [2], [3]]}
        # layout prog_000 folds control 0+1 (15.0 ms) into 12.0; progs 1,2 are shared
        layout = summary({"prog_000": 12.0, "prog_001": 4.04, "prog_002": 1.01},
                         ranges={"prog_000": [11.5, 12.5], "prog_001": [4.0, 4.1], "prog_002": [1.0, 1.02]},
                         p50=[0.8, 0.82, 0.81], spread=0.02,
                         control_sha=control_sha, identity={"tokens_equal": True})

        # the run() helper writes control.json itself, so hash the file as written
        control_path = Path(tmp) / "control.json"
        control_path.write_text(json.dumps(control))
        control_sha = hashlib.sha256(control_path.read_bytes()).hexdigest()
        layout["inputs"]["control_summary_sha256"] = control_sha

        result = run(tmp, control, layout, manifest)
        check("happy rc", result.returncode, 0)
        report = json.loads(result.stdout)
        check("folds counted", report["exec"]["folds"], 1)
        check("saved total", report["exec"]["saved_total_ms"], 3.0)
        # noise = (10.5-9.5)+(5.0-5.0) control + (12.5-11.5) layout = 2.0
        check("noise total", report["exec"]["noise_total_ms"], 2.0)
        check("outside noise", report["exec"]["outside_noise"], True)
        check("shared drift", [s["drift"] for s in report["exec"]["shared_programs"]], [0.01, 0.01])
        check("boots comparable", report["exec"]["boots_comparable"], True)
        check("step ratio", report["step"]["ratio"], round(0.81 / 1.0, 4))
        check("outside spreads", report["step"]["outside_both_spreads"], True)
        check("identity echoed", report["identity"]["layout_identity"], {"tokens_equal": True})

        # saved below noise -> outside_noise false
        slow = dict(layout, timing=dict(layout["timing"], exec_ms_median={
            "prog_000": 13.5, "prog_001": 4.04, "prog_002": 1.01}))
        report = json.loads(run(tmp, control, slow, manifest).stdout)
        check("below noise", report["exec"]["outside_noise"], False)

        # shared drift past 5 percent marks the boots incomparable
        drifted = dict(layout, timing=dict(layout["timing"], exec_ms_median={
            "prog_000": 12.0, "prog_001": 4.4, "prog_002": 1.01}))
        report = json.loads(run(tmp, control, drifted, manifest).stdout)
        check("drift flags boots", report["exec"]["boots_comparable"], False)

        # refusals
        check("not frozen rc", run(tmp, dict(control, frozen=False), layout, manifest).returncode, 2)
        check("failures rc", run(tmp, dict(control, failures=["prog_007"]), layout, manifest).returncode, 2)
        bad_sha = dict(layout, inputs={"control_summary_sha256": "0" * 64})
        check("wrong control sha rc", run(tmp, control, bad_sha, manifest).returncode, 2)
        no_median = dict(layout, timing={"step_p50_s": [1.0]})
        check("not one-arm rc", run(tmp, control, no_median, manifest).returncode, 2)
        check("usage rc", run(tmp, None, None, None, extra_args=["only-one.json"]).returncode, 2)

    if fails:
        print("---")
        for fail in fails:
            print(f"FAIL {fail}")
        print(f"test_qwen_fold_set_compare: {len(fails)} failed")
        return 1
    print("test_qwen_fold_set_compare: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
