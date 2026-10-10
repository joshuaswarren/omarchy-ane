#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Proxy A/B for the resident ane-session without Qwen artifacts: a chain
of --calls-per-step (38) sequential add calls on fixtures/h14-anec/add,
each call's output feeding the next call's input against a constant
second operand, repeated --steps (16) times.

Arm per-call takes the exact ane_call path of tools/qwen_prog_run.py
(flock + ane-run spawn per call); arm resident LOADs the program into one
ane-session and issues the same 38 CALLs per step under the device lock
(--resident-lock call|step). Both arms must end bit-identical to each
other and to the numpy fp16 chain model under the CHK_ADD rounding
semantics (tools/ane-run.c CHK_ADD = ane_f16_round_half_away(f64 a +
f64 b), reproduced exactly in fp16_round_half_away here). Reports
per-arm step-wall p10/p50/p90 and steps/s, records bo_total_bytes
before/after each arm, and aborts on any arm failure, bit mismatch, or
new 'EXCH ... failed' kernel line.

DRY=1 / --dry-run makes no ANE call: it packs, validates and prints the
plan, then exits.
"""
import argparse
import hashlib
import json
import os
import sys
import time
from pathlib import Path

import numpy as np

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))

import qwen_prog_run as qpr
from qwen_prog_run import Refuse
from qwen_resident_ab import bo_total_bytes, idle, kernel_exch_lines, pct

F16 = np.float16
TOOLS = Path(__file__).resolve().parent
FIXTURE = TOOLS.parent / "fixtures" / "h14-anec" / "add"


def fp16_round_half_away(s):
    """The exact CHK_ADD rounding of tools/ane_f16_add.h
    (nearest fp16, ties away from zero, subnormal quantum 2^-24,
    overflow to inf), vectorized; finite-normal positive chains are the
    working domain but every input class is handled."""
    s = np.asarray(s, np.float64)
    out = np.full(s.shape, np.nan)
    fin = np.isfinite(s) & (s != 0)
    m, mag = s[fin], np.abs(s[fin])
    e = np.frexp(mag)[1]
    ulp = np.where(e <= -14, 2.0 ** -24, np.ldexp(np.ones_like(mag), e - 11))
    frac = mag / ulp
    n = np.floor(frac)
    n = np.where(frac - n >= 0.5, n + 1.0, n)  # ties away from zero
    e = e + (n >= 2048.0)
    r = np.where(e <= -14,
                 np.where(n >= 1024.0, 2.0 ** -14, n * ulp),
                 np.where(e > 16, np.inf,
                          n * np.ldexp(np.ones_like(mag), e - 11)))
    out[fin] = np.copysign(r, m)
    out[~fin] = s[~fin]  # signed zeros, +/-inf; NaN -> canonical 0x7e00
    return out.astype(F16)


def model_chain(start, const, adds):
    """The plane after `adds` sequential CHK_ADD adds of `const`."""
    x = start
    for _ in range(adds):
        x = fp16_round_half_away(x.astype(np.float64) + float(const))
    return x


def sha256_plane(arr):
    return hashlib.sha256(np.ascontiguousarray(arr, F16).tobytes()).hexdigest()


def step_stats(walls):
    return {"step_wall_p10_s": round(pct(walls, 0.1), 4),
            "step_wall_p50_s": round(pct(walls, 0.5), 4),
            "step_wall_p90_s": round(pct(walls, 0.9), 4),
            "steps_s": round(len(walls) / sum(walls), 3) if walls else 0.0,
            "steps": len(walls)}


def run_per_call(args, ports_path, ports, x0, const_plane):
    """Arm A: qpr.ane_call per call (flock + ane-run spawn each time)."""
    work = Path(args.out) / "per-call"
    work.mkdir(parents=True, exist_ok=True)
    walls, exec_ms, x = [], [], x0
    for _ in range(args.steps):
        t0 = time.monotonic()
        for _ in range(args.calls):
            status, log, outs = qpr.ane_call(
                Path(args.anec), ports_path, ports,
                {"a": x, "b": const_plane}, work, args.ane_run, args.timeout)
            if status:
                raise SystemExit(f"STOP per-call: ane-run exited {status}: "
                                 f"{log[-500:]}")
            x = outs["y"]
        walls.append(time.monotonic() - t0)
    return x, walls, exec_ms, work


def run_resident(args, ports_path, ports, x0, const_plane):
    """Arm B: one ResidentSession; 38 CALLs per step under its lock."""
    work = Path(args.out) / "resident"
    walls, exec_ms, x = [], [], x0
    sess = qpr.ResidentSession(args.session_bin, work, args.timeout, args.lock)
    try:
        sess.load("add", str(args.anec), str(ports_path))
        for _ in range(args.steps):
            t0 = time.monotonic()
            if args.resident_lock == "step":
                sess.lock()
            for _ in range(args.calls):
                if args.resident_lock == "call":
                    sess.lock()
                outs, ms = sess.call("add", ports, {"a": x, "b": const_plane})
                if args.resident_lock == "call":
                    sess.unlock()
                exec_ms.append(ms)
                x = outs["y"]
            if args.resident_lock == "step":
                sess.unlock()
            walls.append(time.monotonic() - t0)
    finally:
        sess.close()
    return x, walls, exec_ms, work


def arm_record(tag, walls, exec_ms, final, bo_before, bo_after):
    rec = {"tag": tag, "final_sha256": sha256_plane(final),
           "bo_total_bytes_before": bo_before, "bo_total_bytes_after": bo_after,
           "total_wall_s": round(sum(walls), 3), **step_stats(walls)}
    if exec_ms:
        rec["exec_ms_total"] = round(sum(exec_ms), 1)
        rec["exec_ms_p50"] = round(pct(exec_ms, 0.5), 3)
    return rec


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--anec", default=str(FIXTURE / "program-0.anec"))
    ap.add_argument("--ports", default=str(FIXTURE / "ports.json"))
    ap.add_argument("--calls", type=int, default=38,
                    help="calls per step, each output feeds the next input")
    ap.add_argument("--steps", type=int, default=16)
    ap.add_argument("--const", type=float, default=0.25,
                    help="the constant second operand, fp16-exact")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--ane-run", default=str(TOOLS / "ane-run"))
    ap.add_argument("--session-bin", default=str(TOOLS / "ane-session"))
    ap.add_argument("--resident-lock", choices=("call", "step"), default="call")
    ap.add_argument("--lock", default="/var/tmp/ane-run.lock")
    ap.add_argument("--timeout", type=int, default=60)
    ap.add_argument("--idle", type=int, default=60,
                    help="seconds idle before each timed arm")
    ap.add_argument("--out", default="/var/tmp/qproxy-ab")
    ap.add_argument("--bo-growth-mb", type=float, default=512.0)
    ap.add_argument("--dry-run", action="store_true",
                    help="pack, validate, print the plan; no ANE call")
    args = ap.parse_args(argv)
    if os.environ.get("DRY") == "1":
        args.dry_run = True
    if args.calls < 1 or args.steps < 1:
        raise Refuse("--calls and --steps must be positive")
    if args.timeout < 1:
        raise Refuse("--timeout must be positive")
    if F16(args.const).astype(np.float64) != args.const:
        raise Refuse(f"--const {args.const} is not fp16-exact")
    anec = Path(args.anec)
    ports_path = Path(args.ports)
    if not anec.is_file() or not ports_path.is_file():
        raise Refuse(f"missing fixture: {anec} / {ports_path}")
    table = json.loads(ports_path.read_text())
    if table.get("program") != "add":
        raise Refuse(f"{ports_path}: expected program add, "
                     f"found {table.get('program')}")
    ports = qpr.port_map_from_table(table)
    shape = tuple(ports["y"]["shape"])
    x0 = (1.0 + np.random.default_rng(args.seed).random(shape) * 0.5).astype(F16)
    const_plane = np.full(shape, args.const, F16)
    expected = model_chain(x0, args.const, args.calls * args.steps)
    expected_sha = sha256_plane(expected)

    if args.dry_run:
        dry_work = Path(args.out) / "per-call"
        dry_work.mkdir(parents=True, exist_ok=True)
        _, per_call_cmd, _ = qpr.ane_call(anec, ports_path, ports,
                                          {"a": x0, "b": const_plane},
                                          dry_work,
                                          args.ane_run, args.timeout, dry=True)
        print("DRY: no ANE call. The plan:")
        print(f"  fixture {anec} ports {ports_path} shape {list(shape)} "
              f"calls/step {args.calls} steps {args.steps} const {args.const}")
        print(f"  1. idle {args.idle}s; per-call arm: {per_call_cmd} "
              f"x {args.calls}/step")
        print(f"  2. idle {args.idle}s; resident arm: LOAD once, "
              f"{args.calls} CALLs/step "
              f"(lock per {args.resident_lock}): "
              f"{Path(args.session_bin).name} --dev 0 --lock {args.lock}")
        print("  3. compare final outputs bit-for-bit across arms and vs the "
              f"CHK_ADD fp16 model sha {expected_sha[:16]}")
        print("  4. report step-wall p10/p50/p90 and steps/s per arm; abort on "
              "any STOP, mismatch, new EXCH line or BO growth > "
              f"{args.bo_growth_mb} MiB")
        return 0

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    failures, exch_before = [], len(kernel_exch_lines(0))
    results = {}
    for runner, resident in ((run_per_call, False), (run_resident, True)):
        tag = "resident" if resident else "per-call"
        idle(args.idle)
        bo_before = bo_total_bytes()
        try:
            final, walls, exec_ms, work = runner(args, ports_path, ports, x0, const_plane)
            bo_after = bo_total_bytes()
        except SystemExit as stop:
            results[tag] = {"tag": tag, "stopped": str(stop)}
            print(f"FAIL {tag}: {stop}", file=sys.stderr)
            failures.append(f"{tag} arm stopped: {stop}")
            continue
        results[tag] = arm_record(tag, walls, exec_ms, final, bo_before, bo_after)
        np.save(work / "final.npy", final)
        before, after = bo_before, bo_after
        if before is not None and after is not None and \
                after - before > args.bo_growth_mb << 20:
            failures.append(f"{tag}: bo_total_bytes grew "
                            f"{(after - before) >> 20} MiB")
    if all("final_sha256" in r for r in results.values()):
        a, b = results["per-call"], results["resident"]
        if a["final_sha256"] != b["final_sha256"]:
            failures.append(f"final outputs differ: {a['final_sha256']} != "
                            f"{b['final_sha256']}")
        if a["final_sha256"] != expected_sha:
            failures.append(f"per-call != model: {a['final_sha256']} != "
                            f"{expected_sha}")
        if b["final_sha256"] != expected_sha:
            failures.append(f"resident != model: {b['final_sha256']} != "
                            f"{expected_sha}")
    exch = kernel_exch_lines(exch_before)
    if exch:
        failures.append(f"new EXCH kernel lines: {exch[:2]}")
    summary = {"calls_per_step": args.calls, "steps": args.steps,
               "const": args.const, "seed": args.seed,
               "model_sha256": expected_sha, "arms": results,
               "bit_identical": not failures, "failures": failures,
               "exch_lines": exch,
               "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    (out / "summary.json").write_text(json.dumps(summary, indent=1) + "\n")
    print(json.dumps(summary, indent=1))
    for f in failures:
        print(f"FAIL {f}")
    print("PROXY A/B PASS" if not failures else "PROXY A/B FAIL")
    return 1 if failures else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Refuse as e:
        print(f"REFUSE: {e}", file=sys.stderr)
        raise SystemExit(2)
