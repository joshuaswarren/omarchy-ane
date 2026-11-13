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
# /proc/device-tree/compatible holds NUL separated entries on the real
# machine (row m2-cpu-floor-pr411-078): "apple,j414c", "apple,t6021", ...
COMPAT_T6021 = b"apple,j414c\x00apple,t6021\x00apple,arm-platform\x00"


class Tree:
    """One fake sysfs tree plus one fake state dir."""

    def __init__(self, freq=b"702000\n", compat=COMPAT_T6021, floor="1968000"):
        self.base = Path(tempfile.mkdtemp(prefix="ane-floor-proc."))
        self.sysfs = self.base / "sys"
        self.var = self.base / "var"
        (self.sysfs / "cpu4" / "cpufreq").mkdir(parents=True)
        self.var.mkdir()
        self.freq.write_bytes(freq)
        (self.sysfs / "compatible").write_bytes(compat)
        self.floor = floor

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
        e.pop("ANE_CPU_FLOOR", None)
        if self.floor is not None:
            e["ANE_CPU_FLOOR"] = self.floor
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


def serve(tree, seconds, tag, slowclose=False, locksig=False):
    # start_new_session: the watchdog child joins the group, so a full crash
    # (server plus watchdog) is one killpg to a group this test owns.
    argv = [str(SERVE), tag]
    pre = ["slowclose", str(seconds)] if slowclose else (
        ["locksig", str(seconds)] if locksig else [str(seconds)])
    argv[1:1] = pre
    return subprocess.Popen(
        argv, env=tree.env(), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        text=True, start_new_session=True)


def kill_group(proc):
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        pass


def watchdog_pid(proc):
    """The forked watchdog child, via /proc; [] when it has not forked yet
    or already exited."""
    try:
        return Path(f"/proc/{proc.pid}/task/{proc.pid}/children").read_text().split()
    except (FileNotFoundError, ProcessLookupError):
        return []


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
def off_by_default(ok):
    """Without ANE_CPU_FLOOR nothing is read or written: the run skips."""
    t = Tree(floor=None)
    try:
        p = serve(t, 0.4, "floor-off")
        ok(p.stdout.readline().strip() == "FLOOR skipped", "serve reports the skip")
        ok(restored(t), "freq untouched")
        ok(not t.state.exists(), "no state file")
        ok(p.wait(10) == 0, "clean exit")
    finally:
        t.close()




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
def sighup_group(ok):
    """SIGHUP to the whole process group, the shape of a terminal or ssh
    hangup: the run restores -- through its chained handler, or through
    the watchdog, which since #413 runs detached in its own session and
    survives the group delivery."""
    t = Tree()
    try:
        p = serve(t, 60, "floor-hup")
        p.stdout.readline()
        os.killpg(os.getpgid(p.pid), signal.SIGHUP)
        ok(p.wait(10) == -signal.SIGHUP, "SIGHUP still terminates by signal")
        ok(wait_for(lambda: restored(t)), "the group hangup restores")
        ok(wait_for(lambda: not t.state.exists()),
           "the group hangup removes the state")
    finally:
        t.close()


@case
def sigquit_group(ok):
    """SIGQUIT to the whole process group (a group ^\\): the run restores
    through the chained handler or the detached watchdog and still dies
    by SIGQUIT."""
    t = Tree()
    try:
        p = serve(t, 60, "floor-quit")
        p.stdout.readline()
        os.killpg(os.getpgid(p.pid), signal.SIGQUIT)
        ok(p.wait(10) == -signal.SIGQUIT, "SIGQUIT still terminates by signal")
        ok(wait_for(lambda: restored(t)), "the group quit restores")
        ok(wait_for(lambda: not t.state.exists()),
           "the group quit removes the state")
    finally:
        t.close()


@case
def sighup_no_watchdog(ok):
    """SIGHUP to the run alone with the watchdog killed first: nothing
    but the run's chained handler can restore now. Without SIGHUP in the
    chained set the default action kills the run with the floor raised
    and nothing restores it."""
    t = Tree()
    try:
        p = serve(t, 60, "floor-hup-nw")
        p.stdout.readline()
        ok(len(watchdog_pid(p)) == 1, "the watchdog is forked")
        for w in watchdog_pid(p):
            try:
                os.kill(int(w), signal.SIGKILL)
            except ProcessLookupError:
                pass
        p.send_signal(signal.SIGHUP)
        ok(p.wait(10) == -signal.SIGHUP, "SIGHUP still terminates by signal")
        ok(wait_for(lambda: restored(t)), "the handler restores without a watchdog")
        ok(wait_for(lambda: not t.state.exists()),
           "the handler removes the state without a watchdog")
    finally:
        t.close()


@case
def sigquit_no_watchdog(ok):
    """SIGQUIT to the run alone with the watchdog killed first: the
    chained handler restores, the run still dies by SIGQUIT."""
    t = Tree()
    try:
        p = serve(t, 60, "floor-quit-nw")
        p.stdout.readline()
        ok(len(watchdog_pid(p)) == 1, "the watchdog is forked")
        for w in watchdog_pid(p):
            try:
                os.kill(int(w), signal.SIGKILL)
            except ProcessLookupError:
                pass
        p.send_signal(signal.SIGQUIT)
        ok(p.wait(10) == -signal.SIGQUIT, "SIGQUIT still terminates by signal")
        ok(wait_for(lambda: restored(t)), "the handler restores without a watchdog")
        ok(wait_for(lambda: not t.state.exists()),
           "the handler removes the state without a watchdog")
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
        ok(wait_for(lambda: restored(t), timeout=15.0), "watchdog restores after SIGKILL")
        ok(wait_for(lambda: not t.state.exists(), timeout=15.0), "watchdog removes the state")
    finally:
        t.close()


@case
def sigterm_self_locked(ok):
    """SIGTERM while this process itself holds the state lock (the shape of
    an interrupted critical section): the handler's release must not block
    forever on its own flock. It retries bounded and non-blocking, and if
    the lock never frees the run dies by SIGTERM and the watchdog restores
    at death."""
    t = Tree()
    try:
        p = serve(t, 60, "floor-locksig", locksig=True)
        ok(p.stdout.readline().strip() == "FLOOR raised", "serve reports the raise")
        ok(p.stdout.readline().strip() == "LOCK held", "serve holds the state lock")
        term(p)
        try:
            st = p.wait(15)
        except subprocess.TimeoutExpired:
            kill(p)
            p.wait(10)
            ok(False, "SIGTERM while self-locked: the handler hung on the flock")
            return
        ok(st == -signal.SIGTERM, f"SIGTERM still terminates by signal (got {st})")
        ok(wait_for(lambda: restored(t), timeout=10.0), "the floor is restored at death")
        ok(wait_for(lambda: not t.state.exists(), timeout=10.0), "the state is removed at death")
    finally:
        t.close()


@case
def slow_teardown(ok):
    """A dying holder releases its fds (the watchdog pipe among them) inside
    do_exit, BEFORE exit_notify makes it a zombie: the watchdog's first look
    can still see the holder alive, keep the record and leave the floor
    raised. slowclose cuts the pipe while the holder lives and dies only
    later -- the same order, wide enough to lose the race on purpose -- so
    the watchdog must keep reconciling until the holder is really gone."""
    t = Tree()
    try:
        p = serve(t, 0.5, "floor-slow", slowclose=True)
        ok(p.stdout.readline().strip() == "FLOOR raised", "serve reports the raise")
        ok(raised(t), "floor applied while the holder lives")
        p.wait(10)
        ok(wait_for(lambda: restored(t), timeout=15.0),
           "watchdog rides out the slow teardown")
        ok(wait_for(lambda: not t.state.exists(), timeout=15.0),
           "state removed after the slow teardown")
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
    """SIGKILL the run's whole group - systemd's KillMode, the OOM killer,
    a pkill -9 sweep (#413): the watchdog runs detached in its own session
    with the catchable deaths ignored, so it survives the group kill and
    restores the floor itself. The stale record is then crafted by hand,
    because nothing natural leaves one anymore: the next run reconciles
    it before it raises."""
    t = Tree()
    try:
        p = serve(t, 60, "floor-crash")
        p.stdout.readline()
        kill_group(p)
        p.wait(10)
        ok(wait_for(lambda: restored(t), timeout=15.0),
           "the group SIGKILL cannot take the detached watchdog down")
        ok(wait_for(lambda: not t.state.exists(), timeout=15.0),
           "the surviving watchdog removes the state")
        t.state.write_text("saved 702000\npid 4000000\n")
        p2 = serve(t, 0.4, "floor-crash2")
        ok(p2.stdout.readline().strip() == "FLOOR raised", "next run raises after reconcile")
        ok("4000000" not in state_text(t), "stale pid dropped")
        ok(p2.wait(10) == 0 and restored(t), "reconciled run restores normally")
    finally:
        t.close()


@case
def crash_saved_value(ok):
    """After a full group SIGKILL the detached watchdog (#413) restores
    the recorded saved value - here 553000, not the fixture's usual
    702000, so a hard-coded restore cannot hide."""
    t = Tree(freq=b"553000\n")
    try:
        p = serve(t, 60, "floor-saved")
        ok(p.stdout.readline().strip() == "FLOOR raised", "serve reports the raise")
        ok(raised(t), "floor applied")
        kill_group(p)
        p.wait(10)
        ok(wait_for(lambda: t.freq.read_bytes().strip() == b"553000", timeout=15.0),
           "the watchdog restored the saved 553000")
        ok(wait_for(lambda: not t.state.exists(), timeout=15.0),
           "the state is removed")
    finally:
        t.close()


@case
def ane_run_error_exit(ok):
    """The real ane-run, no device: the test overrides are compiled out of
    the production binary (built without ANE_CPU_FLOOR_TESTING), so the
    environment cannot point its sysfs writes or its state directory
    anywhere but the real paths - the fake tree stays untouched and no
    floor line may appear on stderr, however the run exits. The floor
    variable itself is not set: a root run on a real t6021 would write
    the real /sys/devices/system/cpu file, and make check must fail
    closed - tests never touch real sysfs."""
    if not ANE_RUN.exists():
        ok(False, "tools/ane-run is not built (make tools first)")
        return
    t = Tree(floor=None)
    try:
        anec = TOOLS.parent / "fixtures/h14-anec/add/program-0.anec"
        p = subprocess.run(
            [str(ANE_RUN), "--anec", str(anec)],
            env=t.env(), capture_output=True, text=True, timeout=60)
        ok(p.returncode == 1, "ane-run fails on the missing device as before")
        ok("cpu floor" not in p.stderr,
           "ane-run honors no test overrides: no floor line on stderr")
        ok(t.freq.read_bytes() == b"702000\n", "the fake sysfs tree is untouched")
        ok(not t.state.exists(), "no state file under the fake state dir")
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
