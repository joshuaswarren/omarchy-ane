#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 Joshua Warren

"""Mutation proof for ane_cpu_floor.h (host only, no device).

Each mutant is ane_cpu_floor.h with one textual change. The mutated header is
built into a temp dir with copies of the unit suite and the serve binary; the
state-machine faults are run against the unit suite, the signal, watchdog and
refcount faults against one named test_cpu_floor.py case. The control (the
unmutated header) must pass, and every mutant must fail the check it targets:
a mutant that passes is a test that cannot see that fault, and this script
exits 1.
"""

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
HEADER = (TOOLS / "ane_cpu_floor.h").read_text()
CFLAGS = ["-std=gnu99", "-Wall", "-Wextra", "-Werror", "-O2"]

# name -> (case for test_cpu_floor.py or None for the unit suite, old, new)
MUTANTS = {
    # the restore writes the found value, never a constant
    "restore_is_hardcoded": (None,
        """		if (st.npids == 0) {
			w = ane_floor_num(w, st.saved);
			*w++ = '\\n';
			*w = 0;
			ane_floor_write(p->freq, saved);
			unlink(p->state);
		} else {
			ane_floor_state_write(p, &st);
		}
	}
	close(fd);
	if (ane_floor_pipe_w >= 0) {""",
        """		if (st.npids == 0) {
			w = ane_floor_num(w, 702000ULL);
			*w++ = '\\n';
			*w = 0;
			ane_floor_write(p->freq, saved);
			unlink(p->state);
		} else {
			ane_floor_state_write(p, &st);
		}
	}
	close(fd);
	if (ane_floor_pipe_w >= 0) {"""),
    # the floor write in the fresh raise must really happen
    "floor_write_skipped": (None,
        """		if (ane_floor_state_write(p, &st)) {
			close(lockfd);
			return 0;
		}
		if (ane_floor_write(p->freq, floor_out)) {
			/* no write access: silent skip, leave nothing behind */
			unlink(p->state);
			close(lockfd);
			return 0;
		}
		raised_fresh = 1;""",
        """		if (ane_floor_state_write(p, &st)) {
			close(lockfd);
			return 0;
		}
		(void)ane_floor_write(p->freq, floor_out);
		raised_fresh = 1;"""),
    # the saved value must be recorded before the floor is applied
    "saved_never_recorded": (None,
        """			st.saved = saved;
			st.npids = 1;
			st.pids[0] = (int)getpid();
			st.ticks[0] = my_ticks;""",
        """			st.saved = 0;
			st.npids = 1;
			st.pids[0] = (int)getpid();
			st.ticks[0] = my_ticks;"""),
    # the join must add this pid, or the floor loses its last restorer.
    # Replaces the append with a dead pid and drops the list to one entry, so
    # the floor dies with the first holder while the second still serves.
    "join_drops_pid": ("concurrent_refcount",
        "st.pids[st.npids] = (int)getpid();",
        "st.npids = 1; st.pids[0] = 0;"),
    # the SoC gate must walk the NUL separated compatible entries; a strstr on
    # the whole buffer scans only the first entry and never sees "apple,t6021"
    "gate_uses_strstr": (None,
        """	{
		int gotc = ane_floor_read(p->compat, buf, sizeof(buf));
		size_t off;
		int hit = 0;

		if (gotc < 0)
			return 0;
		for (off = 0; off < (size_t)gotc; off += strlen(buf + off) + 1) {
			if (!strcmp(buf + off, "apple,t6021")) {
				hit = 1;
				break;
			}
		}
		if (!hit)
			return 0;
	}""",
        """		if (ane_floor_read(p->compat, buf, sizeof(buf)) < 0 || !strstr(buf, "t6021"))
			return 0;"""),
    # whole-entry equality: a longer entry such as "apple,t60210" is not a match
    "gate_matches_substring": (None,
        """			if (!strcmp(buf + off, "apple,t6021")) {""",
        """			if (strstr(buf + off, "apple,t6021")) {"""),
    # the opt-in gate: with ANE_CPU_FLOOR absent nothing may be read or written
    "optin_gate_removed": (None,
        """	env = getenv(ANE_CPU_FLOOR_ENV);
	if (!env || *env < '1' || *env > '9')
		return 0;
	floor = strtoull(env, &end, 10);
	if (end == env || floor == 0)
		return 0;""",
        """	env = "1968000";
	floor = 1968000ULL;"""),
    # the watchdog child is what covers SIGKILL and crashes
    "watchdog_not_forked": ("sigkill_watchdog",
        "ane_floor_watchdog_start(p);",
        ";"),
    # the watchdog must reconcile after the parent's pipe closes
    "watchdog_never_reconciles": ("sigkill_watchdog",
        "ane_floor_reconcile(p);\n\t\t_exit(0);",
        "_exit(0);"),
    # the SIGINT/SIGTERM handler must release before dying
    "signal_skips_release": (None,
        """	ane_floor_release(&ane_floor_paths);
	ane_floor_held = 0;""",
        """	ane_floor_held = 0;"""),
    # a dead holder (zombie or reused pid) must not keep the floor pinned
    "prune_keeps_zombies": ("sigkill_watchdog",
        """	for (i = 0; i < st->npids; i++) {
		if (!ane_floor_pid_dead(st->pids[i], st->ticks[i])) {""",
        """	for (i = 0; i < st->npids; i++) {
		if (kill((pid_t)st->pids[i], 0) == 0) {"""),
}


def build(work):
    results = []
    # the sources use a quoted include, which resolves in the source directory
    # first: copy them into the work dir so they pick up the mutant header
    for target, src in (("test_cpu_floor", TOOLS / "test_cpu_floor.c"),
                        ("test_cpu_floor_serve", TOOLS / "test_cpu_floor_serve.c")):
        shutil.copy(src, work / src.name)
        r = subprocess.run(["cc", *CFLAGS, "-I", str(work), "-o", str(work / target),
                            str(work / src.name)], capture_output=True, text=True)
        if r.returncode:
            results.append(f"{target} build failed: {r.stderr.strip()[:300]}")
    return results


def run_case(work, case_name):
    env = dict(os.environ)
    env["ANE_RUN_FLOOR_SERVE"] = str(work / "test_cpu_floor_serve")
    env.pop("ANE_RUN_CPUFREQ_ROOT", None)
    env.pop("ANE_RUN_STATE_DIR", None)
    try:
        return subprocess.run([sys.executable, str(TOOLS / "test_cpu_floor.py"), case_name],
                              capture_output=True, text=True, env=env, cwd=str(TOOLS),
                              timeout=90)
    except subprocess.TimeoutExpired:
        return None


def run_unit(work):
    try:
        return subprocess.run([str(work / "test_cpu_floor")], capture_output=True,
                              text=True, timeout=90)
    except subprocess.TimeoutExpired:
        return None


def main():
    failures = 0

    control = Path(tempfile.mkdtemp(prefix="floor-mut-control."))
    shutil.copy(TOOLS / "ane_cpu_floor.h", control / "ane_cpu_floor.h")
    errs = build(control)
    shutil.rmtree(control, ignore_errors=True)
    if errs:
        print("\n".join(errs))
        return 2

    for name, (case_name, old, new) in MUTANTS.items():
        if old not in HEADER:
            print(f"FAIL {name}: anchor not found in ane_cpu_floor.h")
            failures += 1
            continue
        work = Path(tempfile.mkdtemp(prefix=f"floor-mut-{name}."))
        (work / "ane_cpu_floor.h").write_text(HEADER.replace(old, new, 1))
        errs = build(work)
        res = run_unit(work) if errs == [] and case_name is None else (
            run_case(work, case_name) if errs == [] else None)
        killed = res is None or res.returncode != 0
        if killed:
            print(f"ok {name}: killed")
        else:
            print(f"FAIL {name}: mutant passed the check it must fail")
            failures += 1
        shutil.rmtree(work, ignore_errors=True)

    print(f"RESULT: {len(MUTANTS)} mutants, {failures} survivors")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
