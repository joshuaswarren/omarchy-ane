#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""CT mock test: batch_landing_check.py bounds a hung ane-run.

Case 1: a stub ane-run that sleeps forever (with its own child) must be
killed, child included, within the --timeout deadline; the check must
exit nonzero. Case 2: a stub that exits 0 must reach the output
comparison (the bounded path must not change the happy path).
"""
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
TOOL = REPO / "tools" / "batch_landing_check.py"


def wait_gone(pid, deadline=5.0):
    end = time.monotonic() + deadline
    while time.monotonic() < end:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return True
        time.sleep(0.1)
    return False


def run_check(ane_run, work, timeout):
    return subprocess.run(
        [sys.executable, str(TOOL), "--n", "1", "--calls", "1",
         "--timeout", str(timeout), "--ane-run", str(ane_run),
         "--work", str(work)],
        env=dict(os.environ, STUB_PIDFILE=str(work / "stub.pid")),
        capture_output=True, text=True, timeout=120)


def main():
    work = Path(tempfile.mkdtemp(prefix="blc-mock-"))

    # Case 1: hung stub with a child; deadline 2 s.
    stub = work / "ane-run-hang"
    stub.write_text("#!/bin/sh\n"
                    "echo $$ > \"$STUB_PIDFILE\"\n"
                    "sleep 300 & echo $! > \"$STUB_PIDFILE.child\"\n"
                    "wait\n")
    stub.chmod(0o755)
    t0 = time.monotonic()
    r = run_check(stub, work / "hang", 2)
    dt = time.monotonic() - t0
    assert r.returncode != 0, f"hung ane-run must fail the check:\n{r.stdout}"
    assert "TIMEOUT" in r.stdout, r.stdout
    assert dt < 30, f"check returned after {dt:.1f}s; deadline did not bound it"
    # run_check points STUB_PIDFILE inside the per-case work dir.
    hang = (work / "hang").resolve()
    for tag, pidfile in (("stub", hang / "stub.pid"),
                         ("stub child", hang / "stub.pid.child")):
        pid = int(pidfile.read_text())
        assert wait_gone(pid), f"{tag} pid {pid} survived the kill"
    print(f"PASS: hung ane-run + child killed within the 2 s deadline "
          f"(rc {r.returncode}, {dt:.1f}s)")

    # Case 2: well-behaved stub (exit 0, wrong-size output) must reach the
    # comparison, proving the bounded path did not break the normal one.
    stub2 = work / "ane-run-fast"
    outsz = 16384 * 2  # PLANE_WORDS fp16 words for N = 1
    stub2.write_text("#!/bin/sh\n"
                     "prev=\nout=\n"
                     "for a in \"$@\"; do\n"
                     "  if [ \"$prev\" = \"--out\" ]; then out=${a#0=}; fi\n"
                     "  prev=$a\n"
                     "done\n"
                     f"dd if=/dev/zero of=\"$out\" bs={outsz - 1} count=1 "
                     "2>/dev/null\n")
    stub2.chmod(0o755)
    r2 = run_check(stub2, work / "fast", 60)
    assert r2.returncode != 0, f"short output must fail the compare:\n{r2.stdout}"
    assert "mismatched words" in r2.stdout or "!=" in r2.stdout, r2.stdout
    print(f"PASS: fast stub reaches the output comparison (rc "
          f"{r2.returncode})")


if __name__ == "__main__":
    main()
