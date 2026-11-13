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
CFLAGS = ["-std=gnu99", "-Wall", "-Wextra", "-Werror", "-O2",
          "-DANE_CPU_FLOOR_TESTING"]

# name -> (case for test_cpu_floor.py or None for the unit suite, old, new)
MUTANTS = {
    # the restore writes the found value, never a constant (drop_pid: the
    # atexit and signal path)
    "restore_is_hardcoded": (None,
        """		st.npids = live;
		if (st.npids == 0) {
			w = ane_floor_num(w, st.saved);
			*w++ = '\\n';
			*w = 0;
			ane_floor_write(p->freq, saved);
			unlink(p->state);
		} else {
			ane_floor_state_write(p, &st);
		}
	}
	close(fd);""",
        """		st.npids = live;
		if (st.npids == 0) {
			w = ane_floor_num(w, 702000ULL);
			*w++ = '\\n';
			*w = 0;
			ane_floor_write(p->freq, saved);
			unlink(p->state);
		} else {
			ane_floor_state_write(p, &st);
		}
	}
	close(fd);"""),
    # the watchdog's reconcile must also restore the recorded saved value,
    # never a constant: exercised with a non-default saved value
    "reconcile_restore_hardcoded": ("crash_saved_value",
        """		ane_floor_prune(&st);
		if (st.npids == 0) {
			w = ane_floor_num(w, st.saved);""",
        """		ane_floor_prune(&st);
		if (st.npids == 0) {
			w = ane_floor_num(w, 702000ULL);"""),
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
	if (end == env || floor == 0 || *end)
		return 0;""",
        """	env = "1968000";
	floor = 1968000ULL;"""),
    # the watchdog child is what covers SIGKILL and crashes
    "watchdog_not_forked": ("sigkill_watchdog",
        "ane_floor_watchdog_start(p);",
        ";"),
    # the watchdog must reconcile after the parent's pipe closes
    "watchdog_never_reconciles": ("sigkill_watchdog",
        "ane_floor_watchdog_reconcile(p, (int)ppid);\n\t\t_exit(0);",
        "(void)ppid;\n\t\t_exit(0);"),
    # the SIGINT/SIGTERM handler must release before dying
    "signal_skips_release": (None,
        """	ane_floor_release(&ane_floor_paths, 0);
	ane_floor_held = 0;""",
        """	ane_floor_held = 0;"""),
    # the signal-path release must not block on the state lock: the frame the
    # signal interrupted may hold it itself, and a blocking flock there
    # deadlocks the process (hang, floor stays raised)
    "signal_release_blocks": ("sigterm_self_locked",
        "ane_floor_release(&ane_floor_paths, 0);",
        "ane_floor_release(&ane_floor_paths, 1);"),
    # a dead holder (zombie or reused pid) must not keep the floor pinned:
    # the unit suite crafts a live pid whose recorded start time no longer
    # matches, which kill() alone cannot see
    "prune_keeps_zombies": (None,
        """	for (i = 0; i < st->npids; i++) {
		if (!ane_floor_pid_dead(st->pids[i], st->ticks[i])) {""",
        """	for (i = 0; i < st->npids; i++) {
		if (kill((pid_t)st->pids[i], 0) == 0) {"""),
    # trailing junk after the digits is not a number: the gate must reject
    # "1968000kHz" before it touches sysfs
    "env_trailing_junk_ok": (None,
        """	floor = strtoull(env, &end, 10);
	if (end == env || floor == 0 || *end)
		return 0;""",
        """	floor = strtoull(env, &end, 10);
	if (end == env || floor == 0)
		return 0;"""),
    # a full holder table must not join unrecorded: the run sits out, or a
    # later failed write would decrement somebody else's entry
    "holder_full_joins": (None,
        """			if (st.npids == ANE_CPU_FLOOR_HOLDER_MAX) {
				/* the holder table is full: the recorded holders
				 * already keep the floor up, so this run sits
				 * out instead of holding without a record (a
				 * later failed write would then decrement
				 * somebody else's entry) */
				close(lockfd);
				return 0;
			}
			st.pids[st.npids] = (int)getpid();
			st.ticks[st.npids] = my_ticks;
			st.npids++;""",
        """			if (st.npids < ANE_CPU_FLOOR_HOLDER_MAX) {
				st.pids[st.npids] = (int)getpid();
				st.ticks[st.npids] = my_ticks;
				st.npids++;
			}"""),
    # an inherited SIG_IGN must be put back untouched, with the floor intact
    "signal_ign_releases": (None,
        """	o = &ane_floor_old[sig == SIGINT ? 0 : 1];
	if (o->sa_handler == SIG_IGN) {
		/* the run inherited SIG_IGN (nohup, a background job): the
		 * signal is not meant for this process, so put it back and
		 * keep holding the floor */
		sigaction(sig, o, NULL);
		return;
	}
	ane_floor_release(&ane_floor_paths, 0);""",
        """	o = &ane_floor_old[sig == SIGINT ? 0 : 1];
	ane_floor_release(&ane_floor_paths, 0);"""),
    # the fresh branch must read the saved value under the lock: a mutant
    # that trusts a value read before the lock clobbers an administrator
    # change that landed while the begin waited (unit case 13, and case 2's
    # 553000 restore)
    "fresh_saved_stale_value": (None,
        """		/* fresh, no record: the saved value is read under the lock, so
		 * a value that lands in the file while this begin waited for
		 * the lock wins, and a concurrent begin or end cannot feed a
		 * stale one to the shortcut or to the record (an orphaned
		 * floor from a dead run counts only after the stale-record
		 * branch above has been reconciled) */
		if (ane_floor_read(p->freq, buf, sizeof(buf)) < 0) {
			close(lockfd);
			return 0;
		}
		saved = strtoull(buf, &end, 10);
		if (end == buf) {
			close(lockfd);
			return 0;
		}""",
        """		saved = 702000ULL;"""),
    # the watchdog must ride out the dying-parent window: the fds close in
    # do_exit before the dying process is a zombie, so one reconcile at pipe
    # close can still see it alive and leave the floor raised
    "watchdog_reconciles_once": ("slow_teardown",
        """	const struct timespec step = { 0, 20 * 1000 * 1000 };
	struct ane_floor_state st;
	int i, j;

	for (i = 0; i < 1500; i++) {	/* 30 s */
		unsigned long long ticks = 0;
		int found = 0;

		ane_floor_reconcile(p);
		if (ane_floor_state_read(p->state, &st) != 0)
			return;	/* no record left: nothing is held */
		for (j = 0; j < st.npids; j++) {
			if (st.pids[j] == ppid) {
				ticks = st.ticks[j];
				found = 1;
				break;
			}
		}
		if (!found)
			return;	/* this run is out; whoever is left is alive */
		if ((int)getppid() != ppid) {
			/* reparenting happens only at the parent's death, so
			 * the holder is dead even when /proc still argues
			 * alive -- a stranger can reuse the pid within the
			 * same start tick. Drop it directly: the floor must
			 * not wait on a verdict the stranger can spoof. */
			ane_floor_drop_pid(p, ppid, 1);
			return;
		}
		if (ane_floor_pid_dead(ppid, ticks))
			return;	/* gone, a zombie, or its start time reused */
		nanosleep(&step, NULL);
	}""",
        """	if (!ane_floor_pid_dead(ppid, 0))
		ane_floor_reconcile(p);"""),
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
