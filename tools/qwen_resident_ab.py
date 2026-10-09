#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""A/B harness for the staged-Qwen M2 decode: per-call ane-run vs the
resident ane-session, same prompt and new tokens, on the M2.

Each arm runs tools/qwen_m2_decode.py once (60 s idle before it), then the
harness compares the arms: generated token ids and every recorded float
(top1/top2/margin) must match bit for bit, and the raw fp32 logits files
sha256-identical. It reports per-arm step-wall p10/p50/p90 and tok/s, and
records /sys/module/ane_t6021/parameters/bo_total_bytes before and after.

--dry-run prints both commands and the comparison plan; nothing runs and
no ANE call is made. Any arm failure, bit mismatch, new kernel
'EXCH ... failed' line, or bo_total_bytes growth beyond --bo-growth-mb
aborts nonzero (the decode's own STOP rules stop the arm first).
"""
import argparse
import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path

DECODE = Path(__file__).resolve().parent / "qwen_m2_decode.py"
BO_TOTAL = "/sys/module/ane_t6021/parameters/bo_total_bytes"


def sh(cmd, **kw):
    print("+", " ".join(map(str, cmd)), flush=True)
    return subprocess.run(list(map(str, cmd)), **kw)


def bo_total_bytes():
    try:
        return int(Path(BO_TOTAL).read_text().strip())
    except OSError:
        return None


def idle(seconds):
    print(f"idle {seconds}s ...", flush=True)
    time.sleep(seconds)


def run_arm(tag, cmd, out):
    out.mkdir(parents=True, exist_ok=True)
    logits = Path(cmd[cmd.index("--logits-file") + 1])
    logits.unlink(missing_ok=True)
    before = bo_total_bytes()
    idle(args.idle)
    start = time.monotonic()
    run = sh(cmd)
    wall = time.monotonic() - start
    (out / "arm.json").write_text(json.dumps(
        {"tag": tag, "rc": run.returncode, "wall_s": round(wall, 2),
         "bo_total_bytes_before": before,
         "bo_total_bytes_after": bo_total_bytes(),
         "logits_sha256": hashlib.sha256(logits.read_bytes()).hexdigest()
         if logits.exists() else None,
         "argv": " ".join(map(str, cmd))}, indent=1) + "\n")
    return run.returncode


def step_records(out):
    path = out / "results.jsonl"
    steps, prompts = [], []
    for line in path.read_text().splitlines():
        rec = json.loads(line)
        if rec["type"] == "step" and "token_out" in rec:
            steps.append(rec)
        elif rec["type"] == "prompt":
            prompts.append(rec)
    return steps, prompts


def pct(values, q):
    values = sorted(values)
    return values[min(int(q * len(values)), len(values) - 1)]


def compare(a_out, b_out, args):
    a_steps, a_prompts = step_records(a_out)
    b_steps, b_prompts = step_records(b_out)
    failures = []
    if len(a_steps) != len(b_steps) or not a_steps:
        failures.append(f"step count {len(a_steps)} vs {len(b_steps)}")
    for a, b in zip(a_steps, b_steps):
        for key in ("step", "token_in", "token_out", "gen_index", "top1",
                    "top2", "margin", "top2_id"):
            if a.get(key) != b.get(key):
                failures.append(f"step {a['step']} {key}: {a.get(key)!r} != "
                                f"{b.get(key)!r}")
                break
    for a, b in zip(a_prompts, b_prompts):
        for key in ("match", "generated_ids", "reference_ids",
                    "first_divergence"):
            if a.get(key) != b.get(key):
                failures.append(f"prompt {key}: {a.get(key)!r} != {b.get(key)!r}")
    a_sha = (a_out / "logits.f32").exists() and \
        hashlib.sha256((a_out / "logits.f32").read_bytes()).hexdigest()
    b_sha = (b_out / "logits.f32").exists() and \
        hashlib.sha256((b_out / "logits.f32").read_bytes()).hexdigest()
    if a_sha != b_sha:
        failures.append(f"logits sha256 {a_sha} != {b_sha}")

    def stats(steps, out):
        walls = [r["step_wall_s"] for r in steps]
        ane = [r["ane_wall_s"] for r in steps]
        tokens = sum(1 for r in steps if "token_out" in r)
        return {"steps": len(steps), "tok": tokens,
                "tok_s": round(tokens / sum(walls), 3) if walls else 0.0,
                "step_wall_p10_s": pct(walls, 0.1),
                "step_wall_p50_s": pct(walls, 0.5),
                "step_wall_p90_s": pct(walls, 0.9),
                "ane_wall_p50_s": pct(ane, 0.5)}
    summary = {"per_call": stats(a_steps, a_out), "resident": stats(b_steps, b_out),
               "bit_identical": not failures,
               "logits_sha256": a_sha, "failures": failures}
    print(json.dumps(summary, indent=1))
    return failures, summary


def kernel_exch_lines(since_mark):
    out = subprocess.run(["dmesg"], capture_output=True, text=True).stdout
    hits = []
    for line in out.splitlines():
        if "EXCH" in line and "failed" in line:
            hits.append(line)
    return hits[len(hits) - since_mark:] if since_mark else hits


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--prompt", default="p001")
    ap.add_argument("--new-tokens", type=int, default=16)
    ap.add_argument("--manifest", default="/var/tmp/qwen-decode/manifest.json")
    ap.add_argument("--anec-dir", default="/var/tmp/qwen-real-anec-h14")
    ap.add_argument("--ports-dir", default="/var/tmp/qwen-conform-0e2c3743-r2")
    ap.add_argument("--gguf", default="/var/tmp/qwen-decode/Qwen3.8-2B-Q4_K_M.gguf")
    ap.add_argument("--ref", default="/var/tmp/qwen-decode/reference-tokens.json")
    ap.add_argument("--ane-run", default="/var/tmp/inst/tools/ane-run")
    ap.add_argument("--session-bin", default="/var/tmp/inst/tools/ane-session")
    ap.add_argument("--resident-lock", choices=("call", "step"), default="call")
    ap.add_argument("--idle", type=int, default=60)
    ap.add_argument("--out", default="/var/tmp/qres-ab")
    ap.add_argument("--bo-growth-mb", type=float, default=512.0,
                    help="abort if bo_total_bytes grows more than this "
                         "across the A/B (leaked BOs)")
    ap.add_argument("--dry-run", action="store_true",
                    help="print the plan and both commands; run nothing")
    args = ap.parse_args(argv)
    out = Path(args.out)
    a_out, b_out = out / "per-call", out / "resident"

    def arm_cmd(extra, o):
        logits = o / "logits.f32"
        return [sys.executable, DECODE, "--manifest", args.manifest,
                "--anec-dir", args.anec_dir, "--gguf", args.gguf,
                "--ref", args.ref, "--ports-dir", args.ports_dir,
                "--ane-run", args.ane_run, "--prompts", args.prompt,
                "--new-tokens", str(args.new_tokens), "--out", str(o),
                "--logits-file", logits, *extra]
    per_call_cmd = arm_cmd([], a_out)
    resident_cmd = arm_cmd(["--resident", "--session-bin", args.session_bin,
                            "--resident-lock", args.resident_lock], b_out)
    if args.dry_run:
        print("DRY: no ANE call. The plan:")
        print(f"  1. idle {args.idle}s; run: {' '.join(map(str, per_call_cmd))}")
        print(f"  2. idle {args.idle}s; run: {' '.join(map(str, resident_cmd))}")
        print("  3. compare token ids, top1/top2/margin floats and the "
              "logits.f32 sha256; report step p10/p50/p90 and tok/s")
        print("  4. abort nonzero on any mismatch, EXCH line or BO growth "
              f"> {args.bo_growth_mb} MiB")
        return 0

    exch_before = len(kernel_exch_lines(0))
    rc_a = run_arm("per-call", per_call_cmd, a_out)
    rc_b = run_arm("resident", resident_cmd, b_out)
    failures = []
    if rc_a:
        failures.append(f"per-call arm exit {rc_a}")
    if rc_b:
        failures.append(f"resident arm exit {rc_b}")
    bit_failures, summary = compare(a_out, b_out, args)
    failures += bit_failures
    exch = kernel_exch_lines(exch_before)
    if exch:
        failures.append(f"new EXCH kernel lines: {exch[:2]}")
    arms = [json.loads((o / "arm.json").read_text()) for o in (a_out, b_out)]
    for arm in arms:
        before, after = arm["bo_total_bytes_before"], arm["bo_total_bytes_after"]
        if before is not None and after is not None and \
                after - before > args.bo_growth_mb << 20:
            failures.append(f"{arm['tag']}: bo_total_bytes grew "
                            f"{(after - before) >> 20} MiB")
    (out / "summary.json").write_text(json.dumps(
        {"arms": arms, "summary": summary,
         "exch_lines": exch, "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ",
                                                  time.gmtime())},
        indent=1) + "\n")
    for f in failures:
        print(f"FAIL {f}")
    print("A/B PASS" if not failures else "A/B FAIL")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
