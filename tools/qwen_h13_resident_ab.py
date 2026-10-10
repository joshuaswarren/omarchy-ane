#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren
"""H13 (M1 Max) resident-vs-per-call proxy A/B on the H13 add fixture.

The staged Qwen decode set is 2B H14 (M2-only) and no Qwen 0.8B H13 chain
exists, so this proxies the decode-step call pattern: --calls (38) chained
adds per step, each call's output plane feeding the next call's input plane
against a constant second plane, repeated --steps (16) times.

Arm per-call takes the H13 ane-run spawn per call (flock + ane-run --anec ...
--in 0 --in 1 --out 0, index mode, exactly the soak/gate per-call path).
Arm resident LOADs the program once into ane-session-h13 (tools/
ane-session-h13.c, the ABI-1 counterpart of tools/ane-session.c) and issues
the same CALLs under the device lock (--resident-lock call|step). Both arms
must end bit-identical to each other and to the numpy fp16 chain model
(fp16 round-half-away, the CHK_ADD semantics reproduced by
qwen_resident_proxy.fp16_round_half_away).

DRY=1 / --dry-run packs, validates, computes the model sha and prints the
plan; no ANE call, no session start.
"""
import argparse
import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))

from qwen_resident_proxy import fp16_round_half_away, sha256_plane

F16 = np.float16


def model_chain_plane(start, const_plane, adds):
    """CHK_ADD per element: fp64 add, round-half-away back to fp16."""
    x = start
    for _ in range(adds):
        x = fp16_round_half_away(x.astype(np.float64) +
                                 const_plane.astype(np.float64))
    return x


def pct(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, max(0, int(round((len(xs) - 1) * p))))]


def pack_plane_fast(values, tile_bytes, stride):
    surf = np.zeros(tile_bytes, np.uint8)
    raw = np.frombuffer(np.ascontiguousarray(values, F16).tobytes(), np.uint8)
    for i in range(raw.size // 2):
        surf[i * stride:i * stride + 2] = raw[i * 2:i * 2 + 2]
    return surf


def unpack_plane(raw, stride, n):
    vals = np.empty(n, F16)
    for i in range(n):
        vals[i] = np.frombuffer(raw[i * stride:i * stride + 2], F16)[0]
    return vals


def step_stats(walls):
    return {"step_wall_p10_s": round(pct(walls, 0.1), 4),
            "step_wall_p50_s": round(pct(walls, 0.5), 4),
            "step_wall_p90_s": round(pct(walls, 0.9), 4),
            "steps_s": round(len(walls) / sum(walls), 3) if walls else 0.0,
            "steps": len(walls)}


def run_per_call(args, x0, const_plane):
    work = Path(args.out) / "per-call"
    work.mkdir(parents=True, exist_ok=True)
    xf, cf = work / "x.surface", work / "c.surface"
    if not cf.exists():
        pack_plane_fast(const_plane, args.tile_bytes, args.stride).tofile(cf)
    walls, x = [], x0
    for _ in range(args.steps):
        t0 = time.monotonic()
        for _ in range(args.calls):
            pack_plane_fast(x, args.tile_bytes, args.stride).tofile(xf)
            cmd = ["flock", "-w", "600", args.lock, "timeout", str(args.timeout),
                   args.ane_run, "--anec", args.anec,
                   "--in", f"0={xf}", "--in", f"1={cf}",
                   "--out", f"0={xf}.out"]
            run = subprocess.run(cmd, capture_output=True, text=True)
            if run.returncode:
                raise SystemExit(f"STOP per-call: ane-run rc={run.returncode}: "
                                 f"{run.stdout[-300:]} {run.stderr[-300:]}")
            xout = Path(str(xf) + ".out")
            x = unpack_plane(xout.read_bytes(), args.stride, len(x))
        walls.append(time.monotonic() - t0)
    return x, walls


class H13Session:
    """Client for tools/ane-session-h13.c (LOAD/CALL/LOCK/UNLOCK/QUIT)."""

    def __init__(self, session_bin, lock, timeout):
        self.proc = subprocess.Popen(
            [session_bin, "--lock", lock],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, bufsize=0)
        self.in_total = self.out_total = None
        self.timeout = timeout
        self.stderr_tail = b""

    def _line(self):
        ln = self.proc.stdout.readline()
        if not ln:
            raise SystemExit(f"STOP session died: {self.stderr_tail[-400:]}")
        return ln.decode().rstrip("\n")

    def _write_all(self, data):
        # stdin is an unbuffered pipe (bufsize=0): write() may be short.
        view = memoryview(data)
        while view:
            n = self.proc.stdin.write(view)
            if not n:
                raise BrokenPipeError("session stdin closed")
            view = view[n:]

    def _read_exact(self, n):
        # stdout is an unbuffered pipe (bufsize=0): one read() returns
        # whatever is in the pipe, which is short whenever only part of the
        # tool's raw output has landed (the reply line and the payload are
        # separate writes). Loop until the full payload has been read.
        buf = bytearray()
        while len(buf) < n:
            chunk = self.proc.stdout.read(n - len(buf))
            if not chunk:
                raise SystemExit("STOP CALL short output")
            buf.extend(chunk)
        return bytes(buf)

    def cmd(self, text):
        self._write_all(text.encode() + b"\n")
        return self._line()

    def load(self, name, anec):
        rep = self.cmd(f"LOAD {name} {anec}").split()
        if rep[:2] != ["OK", "LOAD"]:
            raise SystemExit(f"STOP LOAD refused: {' '.join(rep)}")
        # OK LOAD <name> <n_in> <n_out> <in_bytes> <out_bytes>
        self.in_total = int(rep[5])
        self.out_total = int(rep[6])
        return rep

    def call(self, name, in_bytes):
        self._write_all(f"CALL {name}\n".encode() + in_bytes)
        rep = self._line().split()
        if rep[:2] != ["OK", "CALL"]:
            raise SystemExit(f"STOP CALL failed: {' '.join(rep)}")
        out = self._read_exact(self.out_total)
        return out, int(rep[3])

    def lock(self):
        return self.cmd("LOCK")

    def unlock(self):
        return self.cmd("UNLOCK")

    def close(self):
        try:
            self.cmd("QUIT")
        except SystemExit:
            pass
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def run_resident(args, x0, const_plane):
    work = Path(args.out) / "resident"
    work.mkdir(parents=True, exist_ok=True)
    sess = H13Session(args.session_bin, args.lock, args.timeout)
    walls, exec_us, x = [], [], x0
    try:
        sess.load("add", args.anec)
        for _ in range(args.steps):
            t0 = time.monotonic()
            if args.resident_lock == "step":
                sess.lock()
            for _ in range(args.calls):
                if args.resident_lock == "call":
                    sess.lock()
                out, us = sess.call(
                    "add", pack_plane_fast(x, args.tile_bytes, args.stride)
                    .tobytes() + pack_plane_fast(
                        const_plane, args.tile_bytes, args.stride).tobytes())
                if args.resident_lock == "call":
                    sess.unlock()
                exec_us.append(us)
                x = unpack_plane(out, args.stride, len(x))
            if args.resident_lock == "step":
                sess.unlock()
            walls.append(time.monotonic() - t0)
    finally:
        sess.close()
    return x, walls, exec_us


def dmesg_fault_lines():
    try:
        d = subprocess.run(["dmesg"], capture_output=True, text=True,
                           timeout=30).stdout
    except Exception:
        return -1
    import re
    return len(re.findall(r"DART fault|translation fault|EXCH failed|"
                          "quarantin|completion-wait", d))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    here = Path(__file__).resolve().parent
    ap.add_argument("--anec", default=str(here / "program-0.anec"))
    ap.add_argument("--ports", default=str(here / "h13-ports.json"))
    ap.add_argument("--calls", type=int, default=38)
    ap.add_argument("--steps", type=int, default=16)
    ap.add_argument("--ane-run",
                    default=str(Path.home() / "scratch/w73/gap/tools/ane-run"))
    ap.add_argument("--session-bin",
                    default=str(here / "ane-session-h13"))
    ap.add_argument("--lock", default="/var/tmp/ane-run.lock")
    ap.add_argument("--resident-lock", choices=["call", "step"], default="call")
    ap.add_argument("--timeout", type=int, default=60)
    ap.add_argument("--out", default=str(here / "run"))
    ap.add_argument("--x0-seed", type=int, default=20261009)
    ap.add_argument("--const", type=float, default=0.25)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)

    ports = json.loads(Path(args.ports).read_text())
    args.tile_bytes = int(ports["tile_bytes"])
    args.stride = int(ports["stride"])
    n = int(ports["valid_planes"])
    rng = np.random.default_rng(args.x0_seed)
    x0 = np.round(rng.uniform(-2.0, 2.0, n), 3).astype(F16)
    const_plane = np.full(n, F16(args.const), F16)
    # Both arms chain x through every step, so the run applies calls * steps adds (608 by default), not one step's worth.
    expect = model_chain_plane(x0, const_plane, args.calls * args.steps)
    expect_sha = sha256_plane(expect)

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    plan = {
        "anec": args.anec, "calls": args.calls, "steps": args.steps,
        "tile_bytes": args.tile_bytes, "stride": args.stride,
        "resident_lock": args.resident_lock,
        "model_expected_sha256": expect_sha,
        "dry": bool(args.dry_run),
    }
    print("PLAN " + json.dumps(plan, sort_keys=True))
    if args.dry_run:
        print("DRY: nothing was run on the ANE; no session started")
        return 0

    dm0 = dmesg_fault_lines()
    x_pc, walls_pc = run_per_call(args, x0, const_plane)
    x_rs, walls_rs, exec_us = run_resident(args, x0, const_plane)
    dm1 = dmesg_fault_lines()
    sha_pc, sha_rs = sha256_plane(x_pc), sha256_plane(x_rs)

    checks = {
        "per_call_matches_model": sha_pc == expect_sha,
        "resident_matches_model": sha_rs == expect_sha,
        "arms_bit_identical": sha_pc == sha_rs,
        "dmesg_no_growth": dm0 >= 0 and dm1 >= 0 and dm1 == dm0,
    }
    rec = {
        "model_expected_sha256": expect_sha,
        "per_call": {"final_sha256": sha_pc, **step_stats(walls_pc)},
        "resident": {"final_sha256": sha_rs,
                     "exec_us_p50": pct(exec_us, 0.5) if exec_us else 0,
                     **step_stats(walls_rs)},
        "checks": checks,
        "verdict": "PASS" if all(checks.values()) else "FAIL",
    }
    (out / "ab.json").write_text(json.dumps(rec, indent=2, sort_keys=True))
    print(json.dumps(rec, indent=2, sort_keys=True))
    print("VERDICT=" + rec["verdict"])
    return 0 if rec["verdict"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
