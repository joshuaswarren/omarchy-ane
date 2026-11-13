#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Process-level host tests for ane_cpu_floor.h (no device): signals, the
watchdog child, concurrent holders, stale-state reconcile, and the real
ane-run error-exit path. The state machine cases live in test_cpu_floor.c.

usage: python3 tools/test_cpu_floor.py [CASE ...]   (default: every case)
output: one ok/FAIL line per case on the terminal, then a RESULT summary.
"""

import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
SERVE = Path(os.environ.get("ANE_RUN_FLOOR_SERVE") or TOOLS / "test_cpu_floor_serve")
ANE_RUN = TOOLS / "ane-run"
COMPAT_T6021 = b"apple,j414s\napple,t6021\n"


class Tree:
    """One fake sysfs tree plus one fake state dir."""

    def __init__(self, freq=b"702000\n", compat=COMPAT_T6021):
        self.base = Path(tempfile.mkdtemp(prefix="ane-floor-proc."))
        self.sysfs = self.base / "sys"
        self.var = self.base / "var"
        (self.sysfs / "cpu4" / "cpufreq").mkdir(parents=True)
        self.var.mkdir()
        self.freq.write_bytes(freq)
        (self.sysfs / "compatible").write_bytes(compat)

    @property
    def freq(self):
        return self.sysfs / "cpu4" / "cpufreq" / "scaling_min_freq"

    @property
    def state(self):
        return self.var / "ane-run.state"

    def env(self):
        e = dict(os.environ)
        e["ANE_RUN_CPUFREQ_ROOT"] = str(self.sysfs)
        e["ANE_RUN_STATE_DIR"] = str(self.var)
        return e

    def close(self):
        shutil.rmtree(self.base, ignore_errors=True)


def wait_for(pred, timeout=5.0, step=0.05):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pred():
            return True
        time.sleep(step)
    return pred()


def serve(tree, seconds, tag):
    # start_new_session: the watchdog child joins the group, so a full crash
    # (server plus watchdog) is one killpg to a group this test owns.
    return subprocess.Popen(
        [str(SERVE), str(seconds), tag],
        env=tree.env(), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        text=True, start_new_session=True)


def kill_group(proc):
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass


def raised(tree):
    return tree.freq.read_bytes().strip() == b"1968000"


def restored(tree):
    return tree.freq.read_bytes().strip() == b"702000"


def state_text(tree):
    return tree.state.read_text() if tree.state.exists() else ""


def term(proc):
    try:
        proc.send_signal(signal.SIGTERM)
    except ProcessLookupError:
        pass


def kill(proc):
    try:
        proc.send_signal(signal.SIGKILL)
    except ProcessLookupError:
        pass


CASES = {}


def case(fn):
    CASES[fn.__name__] = fn
    return fn


@case
def normal_exit(ok):
    t = Tree()
    try:
        p = serve(t, 0.4, "floor-normal")
        ok(p.stdout.readline().strip() == "FLOOR raised", "serve reports the raise")
        ok(raised(t), "floor applied while serving")
        ok(p.wait(10) == 0, "normal exit is clean")
        ok(restored(t), "normal exit restores")
        ok(not t.state.exists(), "state removed")
    finally:
        t.close()


@case
def sigterm(ok):
    t = Tree()
    try:
        p = serve(t, 60, "floor-term")
        p.stdout.readline()
        term(p)
        ok(p.wait(10) == -signal.SIGTERM, "SIGTERM still terminates by signal")
        ok(wait_for(lambda: restored(t)), "SIGTERM restores")
        ok(not t.state.exists(), "SIGTERM removes the state")
    finally:
        t.close()


@case
def sigint(ok):
    t = Tree()
    try:
        p = serve(t, 60, "floor-int")
        p.stdout.readline()
        try:
            p.send_signal(signal.SIGINT)
        except ProcessLookupError:
            pass
        ok(p.wait(10) == -signal.SIGINT, "SIGINT still terminates by signal")
        ok(wait_for(lambda: restored(t)), "SIGINT restores")
    finally:
        t.close()


@case
def sigkill_watchdog(ok):
    t = Tree()
    try:
        p = serve(t, 60, "floor-kill")
        p.stdout.readline()
        kill(p)
        p.wait(10)
        ok(wait_for(lambda: restored(t)), "watchdog restores after SIGKILL")
        ok(wait_for(lambda: not t.state.exists()), "watchdog removes the state")
    finally:
        t.close()


@case
def concurrent_refcount(ok):
    t = Tree()
    try:
        a = serve(t, 60, "floor-con-a")
        b = serve(t, 60, "floor-con-b")
        a.stdout.readline()
        b.stdout.readline()
        ok(raised(t), "both holders share one raise")
        kill(a)
        a.wait(10)
        time.sleep(1.0)
        ok(raised(t), "the floor outlives one killed holder")
        ok(t.state.exists(), "state kept while a holder lives")
        term(b)
        b.wait(10)
        ok(wait_for(lambda: restored(t)), "the last holder restores")
        ok(wait_for(lambda: not t.state.exists()), "state removed by the last holder")
    finally:
        t.close()


@case
def crash_all_stale(ok):
    """SIGKILL the server and its watchdog: the next run reconciles the stale state."""
    t = Tree()
    try:
        p = serve(t, 60, "floor-crash")
        p.stdout.readline()
        kill_group(p)
        p.wait(10)
        time.sleep(0.3)
        ok(t.state.exists() and raised(t), "orphaned state survives the full crash")
        p2 = serve(t, 0.4, "floor-crash2")
        ok(p2.stdout.readline().strip() == "FLOOR raised", "next run raises after reconcile")
        ok("4000000" not in state_text(t), "stale pid dropped")
        ok(p2.wait(10) == 0 and restored(t), "reconciled run restores normally")
    finally:
        t.close()


@case
def ane_run_error_exit(ok):
    """The real ane-run, no device: the raise still unwinds on the error exit."""
    if not ANE_RUN.exists():
        ok(False, "tools/ane-run is not built (make tools first)")
        return
    t = Tree()
    try:
        anec = TOOLS.parent / "fixtures/h14-anec/add/program-0.anec"
        p = subprocess.run(
            [str(ANE_RUN), "--anec", str(anec)],
            env=t.env(), capture_output=True, text=True, timeout=60)
        ok(p.returncode == 1, "ane-run fails on the missing device as before")
        ok(restored(t), "the error exit restored the saved value")
        ok(not t.state.exists(), "the error exit removed the state")
    finally:
        t.close()


def main():
    names = sys.argv[1:] or list(CASES)
    fails = 0
    checks = 0

    for name in names:
        stats = [0, 0]  # checks, failures

        def ok(good, msg, _stats=stats):
            _stats[0] += 1
            if not good:
                _stats[1] += 1
                print(f"FAIL {msg}", flush=True)
            return good

        if not SERVE.exists():
            print(f"FAIL {name}: {SERVE.name} is not built (make test_cpu_floor_serve)", flush=True)
            fails += 1
            continue
        CASES[name](ok)
        checks += 1
        fails += stats[1]
        print(f"{'ok' if not stats[1] else 'FAIL'} {name}: {stats[0] - stats[1]}"
              f"/{stats[0]} checks", flush=True)

    print(f"RESULT: {checks} cases, {fails} failures", flush=True)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
