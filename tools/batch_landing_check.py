#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Landing check for the N-add batch packages (runs on the M2).

Per call, with fresh seeded inputs: pack the stacked [N x 32768 B]
input surfaces, run one CALL of the batch package through ane-run,
read the output, and compare every output plane bit-exact against the
device-proven fp16 half-away add reference (tools/ane_f16_add.h
semantics, shared with check_add_batch.py).

Takes /var/tmp/ane-run.lock itself around every ane-run; never wrap
this script in another flock (that deadlock cost two boots).

usage: batch_landing_check.py --n N [--calls K] [--seed S]
                              [--anec FILE] [--ane-run BIN] [--work DIR]
                              [--timeout SECS]
exit 0 iff every call landed all N planes bit-exact.
"""
import argparse
import fcntl
import os
import signal
import subprocess
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_add_batch import PLANE_WORDS, add_ref  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
LOCK = "/var/tmp/ane-run.lock"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--n", type=int, required=True)
    ap.add_argument("--calls", type=int, default=12)
    ap.add_argument("--seed", type=int, default=17000)
    ap.add_argument("--anec", type=Path,
                    default=REPO / "fixtures/h14-anec/add-batch-N"
                    / "program-0.anec")
    ap.add_argument("--ane-run", type=Path,
                    default=Path(__file__).resolve().parent / "ane-run")
    ap.add_argument("--work", type=Path, default=None)
    ap.add_argument("--timeout", type=float, default=60.0,
                    help="per-CALL ane-run deadline in seconds; expiry "
                         "kills the whole ane-run process group and "
                         "fails the check (a hung CALL must not hold "
                         "/var/tmp/ane-run.lock)")
    args = ap.parse_args(argv)
    anec = Path(str(args.anec).replace("add-batch-N",
                                       f"add-batch-{args.n}"))
    work = args.work or Path(f"/var/tmp/batch-lever/land-{args.n}")
    work.mkdir(parents=True, exist_ok=True)

    for call in range(args.calls):
        rng = np.random.default_rng(args.seed + 1000 * args.n + call)
        ins = []
        for k in range(2):
            x = np.zeros(PLANE_WORDS * args.n, dtype=np.float16)
            for i in range(args.n):
                x[i * PLANE_WORDS:(i + 1) * PLANE_WORDS:32] = \
                    rng.uniform(-8, 8, 512).astype(np.float16)
            name = work / f"in-{k}.f16"
            x.tofile(name)
            ins.append(x)
        want = np.zeros(PLANE_WORDS * args.n, dtype=np.uint16)
        for i in range(args.n):
            lo, hi = i * PLANE_WORDS, (i + 1) * PLANE_WORDS
            want[lo:hi] = add_ref(ins[0][lo:hi].view(np.uint16),
                                  ins[1][lo:hi].view(np.uint16))
        out = work / "out.f16"
        cmd = [str(args.ane_run), "--anec", str(anec),
               "--in", f"0={work / 'in-0.f16'}",
               "--in", f"1={work / 'in-1.f16'}",
               "--out", f"0={out}"]
        with open(LOCK, "w") as lockf:
            fcntl.flock(lockf, fcntl.LOCK_EX)
            # start_new_session: a hung ane-run may have children; kill
            # the whole process group so nothing outlives the deadline
            # while this process still holds the lock.
            p = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True,
                                 start_new_session=True)
            try:
                r_out, r_err = p.communicate(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(p.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                p.wait()
                print(f"land n={args.n} call {call}: TIMEOUT after "
                      f"{args.timeout:g}s, killed ane-run process group "
                      f"{p.pid}")
                return 1
        if p.returncode:
            print(f"land n={args.n} call {call}: ane-run rc "
                  f"{p.returncode}: {r_err.strip()[:300]}")
            return 1
        got = np.fromfile(out, dtype=np.uint16)
        if got.size != want.size:
            print(f"land n={args.n} call {call}: output {got.size} words "
                  f"!= {want.size}")
            return 1
        bad = np.flatnonzero(got != want)
        if bad.size:
            w = int(bad[0])
            print(f"land n={args.n} call {call}: {bad.size} mismatched "
                  f"words, first word {w} (plane {w // PLANE_WORDS}, "
                  f"lane {(w % PLANE_WORDS) // 32}): got "
                  f"{got[w]:#06x} want {want[w]:#06x}")
            return 1
        print(f"land n={args.n} call {call}: {args.n}/{args.n} planes "
              f"bit-exact ({want.size} words)")
    print(f"land n={args.n}: {args.calls} calls, all outputs bit-exact")
    return 0


if __name__ == "__main__":
    sys.exit(main())
