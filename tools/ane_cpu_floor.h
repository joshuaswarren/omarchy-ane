// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

#ifndef ANE_CPU_FLOOR_H
#define ANE_CPU_FLOOR_H

/*
 * ane_cpu_floor.h -- hold a CPU clock floor under a serving ane-run (t6021 only).
 *
 * With ANE_CPU_FLOOR=<kHz> set (for example ANE_CPU_FLOOR=1968000, the value
 * the rows measure), ane-run raises the t6021 (M2 Max) P cluster's minimum
 * clock (policy4, cpus 4-7, scaling_min_freq) to <kHz> while it serves so the
 * schedutil governor cannot idle the P cores between ANE submissions; the
 * M2 Max's second P cluster (policy8, cpus 8-11) is not floored, so work the
 * scheduler places there runs ungated; with the variable unset, zero, or not
 * a number -- trailing junk included, "1968000kHz" is not a number -- nothing
 * under sysfs is read or written at all. Only scaling_min_freq is written:
 * the governor is never changed, and the value found in the file is saved and
 * restored, never assumed. Measured in row m2-minfreq-interleaved-074
 * (default and 1968000 alternating in one row, linked program, cpu4 pinned,
 * schedutil, one 14-inch M2 Max, 840 s): default 702000 kHz n=214000 p50/p90/p99
 * 1.907/2.066/2.242 ms; 1968000 kHz n=213000 1.874/1.939/1.994 ms (p90
 * -6.1 percent, p99 -11.1 percent, p50 -1.7 percent). The minimum was
 * restored to 702000 at the end of the row. The power cost is row 072
 * (hwmon4 "Total System Power", 1 Hz, medians over about 359 samples per arm,
 * one 14-inch M2 Max, whole machine): idle +1.30 W and serve +1.94 W at the
 * floor, which is why the floor is held only while ane-run serves. The means
 * are not usable: rare spikes of about 100 W hit both arms and their source
 * is not identified yet (a follow-up row looks for it). The ring program is
 * row 075, still to run.
 *
 * Every exit path restores:
 *   - normal exit: atexit restores in this process;
 *   - SIGINT/SIGTERM: a chained handler restores, then gives the signal back
 *     so the exit status still says how the run died; an inherited SIG_IGN
 *     (nohup, a background job) is put back untouched and the floor survives
 *     the delivery;
 *   - SIGKILL or a crash: a watchdog child forked at begin watches a pipe; the
 *     kernel closes the pipe when the parent dies, and the child restores from
 *     the state file once the parent is really gone -- a dying process
 *     releases its fds (the pipe among them) inside do_exit, before
 *     exit_notify makes it a zombie, so the first look at the pid can still
 *     say alive;
 *   - a watchdog that dies too (or a run that was killed before it could
 *     record anything): the state file survives as the stale-state record and
 *     the next begin reconciles it before raising again.
 *
 * Concurrent ane-run instances share one state file under a flock. The state
 * carries every holder pid with its /proc start time; a holder whose pid is
 * gone, is a zombie, or whose start time no longer matches is dead, so a
 * killed run cannot pin the floor (a pid that was reused after a fast crash
 * belongs to a stranger and counts as dead).
 *
 * Privilege: nothing in the package grants write access to cpufreq sysfs
 * today. The accel node is mode 0666 through systemd's 50-udev-default.rules,
 * but sysfs is not, and the package installs no sudoers rule, udev rule or
 * unit for it. The floor therefore applies only when this process can already
 * write scaling_min_freq (a run under sudo or a root service); without write
 * access, without policy4, or on any SoC whose device tree does not name
 * t6021, this is a silent no-op and ane-run never fails because of it. sysfs
 * is never made world-writable here.
 *
 * Host-test overrides (never set in production):
 *   ANE_RUN_CPUFREQ_ROOT  replaces /sys/devices/system/cpu with a fake tree;
 *                         the SoC gate then reads <root>/compatible;
 *   ANE_RUN_STATE_DIR     replaces /run/ane-run (lock and state files).
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define ANE_CPU_FLOOR_SYSFS_ENV "ANE_RUN_CPUFREQ_ROOT"
#define ANE_CPU_FLOOR_STATE_ENV "ANE_RUN_STATE_DIR"
#define ANE_CPU_FLOOR_ENV "ANE_CPU_FLOOR"
#define ANE_CPU_FLOOR_CPU "cpu4"	/* policy4: the t6021 P cluster, cpus 4-7 */
#define ANE_CPU_FLOOR_KHZ_DEFAULT "1968000"	/* kHz, the value the rows measure */
#define ANE_CPU_FLOOR_HOLDER_MAX 64

struct ane_floor_paths {
	char freq[512];		/* scaling_min_freq of policy4 */
	char compat[512];	/* the SoC gate */
	char dir[512];		/* state directory */
	char state[512];	/* state file */
	char lock[512];		/* lock file */
};

struct ane_floor_state {
	unsigned long long saved;
	int pids[ANE_CPU_FLOOR_HOLDER_MAX];
	unsigned long long ticks[ANE_CPU_FLOOR_HOLDER_MAX];	/* /proc start time */
	int npids;
};

static struct ane_floor_paths ane_floor_paths;
static struct sigaction ane_floor_old[2];
static int ane_floor_held;	/* this process is a live holder */
static int ane_floor_pipe_w = -1;

static void ane_cpu_floor_end(void);

/* A process that fork()ed while holding the floor must call this in the child
 * before it does anything else: the child inherits the participation flag and
 * the watchdog pipe but owns neither. It holds no state-file membership, so
 * its own begin/end start fresh, and its copy of the pipe closes without
 * releasing the parent's watchdog. */
static void __attribute__((unused)) ane_cpu_floor_forked_child(void)
{
	ane_floor_held = 0;
	if (ane_floor_pipe_w >= 0) {
		close(ane_floor_pipe_w);
		ane_floor_pipe_w = -1;
	}
}

/* Append a decimal number: used on the signal path, where snprintf is not
 * async-signal-safe. Returns the new end pointer. */
static char *ane_floor_num(char *dst, unsigned long long v)
{
	char tmp[24];
	int n = 0;

	do {
		tmp[n++] = (char)('0' + (v % 10));
		v /= 10;
	} while (v);
	while (n)
		*dst++ = tmp[--n];
	return dst;
}

/* Join a base path and a leaf without printf: paths stay bounded and
 * -Wformat-truncation stays quiet. Used before any signal can arrive. */
static int ane_floor_join(char *dst, size_t len, const char *base, const char *leaf)
{
	size_t n = strlen(base);

	if (n + strlen(leaf) + 2 > len)
		return -1;
	memcpy(dst, base, n);
	dst[n++] = '/';
	memcpy(dst + n, leaf, strlen(leaf) + 1);
	return 0;
}

static int ane_floor_read(const char *path, char *buf, size_t len)
{
	int fd, got;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;
	got = (int)read(fd, buf, len - 1);
	close(fd);
	if (got < 0)
		return -1;
	buf[got] = 0;
	return got;
}

static int ane_floor_write(const char *path, const char *s)
{
	int fd, ok;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -1;
	ok = write(fd, s, strlen(s)) == (ssize_t)strlen(s) ? 0 : -1;
	close(fd);
	return ok;
}

/* Take the state lock. mkdir(2) is not async-signal-safe, so the signal path
 * calls this with make_dir 0: the directory always exists while a holder
 * lives, and without it there is nothing to release. */
static int ane_floor_lock_at(const struct ane_floor_paths *p, int make_dir)
{
	int fd;

	if (make_dir && mkdir(p->dir, 0755) && errno != EEXIST)
		return -1;
	fd = open(p->lock, O_RDWR | O_CREAT, 0644);
	if (fd < 0)
		return -1;
	if (flock(fd, LOCK_EX)) {
		close(fd);
		return -1;
	}
	return fd;
}

static int ane_floor_lock(const struct ane_floor_paths *p)
{
	return ane_floor_lock_at(p, 1);
}

/* The start time of a pid, /proc/<pid>/stat field 22 in clock ticks. 0 when
 * the pid does not exist or the field cannot be read. */
static unsigned long long ane_floor_start_ticks(int pid)
{
	char path[48], buf[1024], *p, *tok;
	unsigned long long v = 0;
	int fd, n, i;

	p = path;
	memcpy(p, "/proc/", 6);
	p += 6;
	p = ane_floor_num(p, (unsigned long long)pid);
	memcpy(p, "/stat", 6);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	n = (int)read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = 0;
	p = strrchr(buf, ')');
	if (!p)
		return 0;
	/* field 22 is the 20th token after field 2; field 3 (the state letter)
	 * starts right after ") " */
	tok = p + 2;
	for (i = 3; i <= 22 && tok; i++) {
		if (i == 22) {
			v = strtoull(tok, NULL, 10);
			break;
		}
		while (*tok && *tok != ' ')
			tok++;
		while (*tok == ' ')
			tok++;
		if (!*tok)
			tok = NULL;
	}
	return v;
}

/* A holder is dead when the pid is gone, is a zombie (exited, not yet
 * reaped: kill(pid, 0) still answers for it), or its start time no longer
 * matches the record (the pid was reused by a stranger). */
static int ane_floor_pid_dead(int pid, unsigned long long ticks)
{
	char path[48], buf[1024], *p, *tok;
	int fd, n, i;
	unsigned long long cur = 0;

	if (kill((pid_t)pid, 0)) {
		if (errno == ESRCH)
			return 1;
		return 0;	/* EPERM: alive, owned by someone else */
	}
	p = path;
	memcpy(p, "/proc/", 6);
	p += 6;
	p = ane_floor_num(p, (unsigned long long)pid);
	memcpy(p, "/stat", 6);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return errno == ENOENT;
	n = (int)read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return 0;
	buf[n] = 0;
	p = strrchr(buf, ')');
	if (!p)
		return 0;
	if (p[1] == ' ' && p[2] == 'Z')
		return 1;
	tok = p + 2;
	for (i = 3; i <= 22 && tok; i++) {
		if (i == 22) {
			cur = strtoull(tok, NULL, 10);
			break;
		}
		while (*tok && *tok != ' ')
			tok++;
		while (*tok == ' ')
			tok++;
		if (!*tok)
			tok = NULL;
	}
	return ticks != 0 && cur != 0 && cur != ticks;
}

static void ane_floor_prune(struct ane_floor_state *st)
{
	int i, live = 0;

	for (i = 0; i < st->npids; i++) {
		if (!ane_floor_pid_dead(st->pids[i], st->ticks[i])) {
			st->pids[live] = st->pids[i];
			st->ticks[live] = st->ticks[i];
			live++;
		}
	}
	st->npids = live;
}

static int ane_floor_state_read(const char *path, struct ane_floor_state *st)
{
	char buf[8192], *line, *end;

	memset(st, 0, sizeof(*st));
	if (ane_floor_read(path, buf, sizeof(buf)) < 0)
		return -1;
	line = strstr(buf, "saved ");
	if (!line)
		return -1;
	st->saved = strtoull(line + 6, &end, 10);
	if (end == line + 6)
		return -1;
	line = buf;
	while ((line = strstr(line, "\npid ")) != NULL &&
	       st->npids < ANE_CPU_FLOOR_HOLDER_MAX) {
		long v = strtol(line + 5, &end, 10);

		if (end != line + 5 && v > 0) {
			st->pids[st->npids] = (int)v;
			st->ticks[st->npids] =
				strtoull(end, NULL, 10);	/* 0 when absent */
			st->npids++;
		}
		line = end;
	}
	return 0;
}

static int ane_floor_state_write(const struct ane_floor_paths *p, const struct ane_floor_state *st)
{
	char buf[1024], tmp[512], *w = buf;
	int i, r, fd;

	/* write to a sibling and rename: a writer killed mid-write (or two
	 * writers racing) can never leave a torn state file behind */
	memcpy(w, "saved ", 6);
	w += 6;
	w = ane_floor_num(w, st->saved);
	*w++ = '\n';
	for (i = 0; i < st->npids; i++) {
		memcpy(w, "pid ", 4);
		w += 4;
		w = ane_floor_num(w, (unsigned long long)st->pids[i]);
		if (st->ticks[i]) {
			*w++ = ' ';
			w = ane_floor_num(w, st->ticks[i]);
		}
		*w++ = '\n';
	}
	if (ane_floor_join(tmp, sizeof(tmp), p->dir, "ane-run.state.tmp"))
		return -1;
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -1;
	r = write(fd, buf, (size_t)(w - buf)) == (ssize_t)(w - buf) ? 0 : -1;
	close(fd);
	if (r)
		return -1;
	if (rename(tmp, p->state)) {
		unlink(tmp);
		return -1;
	}
	return 0;
}


/* Drop dead holders; when none are left, give the saved value back and clear
 * the record. Called from begin (a stale file) and from the watchdog (the
 * parent died) -- never from a signal handler. */
static void ane_floor_reconcile(const struct ane_floor_paths *p)
{
	struct ane_floor_state st;
	char saved[32], *w = saved;
	int fd = ane_floor_lock(p);

	if (fd < 0)
		return;
	if (ane_floor_state_read(p->state, &st) == 0) {
		ane_floor_prune(&st);
		if (st.npids == 0) {
			w = ane_floor_num(w, st.saved);
			*w++ = '\n';
			*w = 0;
			ane_floor_write(p->freq, saved);
			unlink(p->state);
		} else {
			ane_floor_state_write(p, &st);
		}
	}
	close(fd);
}

/* The watchdog's reconcile: the dying parent releases its fds (the pipe
 * among them) from do_exit, BEFORE exit_notify makes it a zombie, so for the
 * rest of its exit kill(pid, 0) still answers and /proc still shows a live
 * state. One reconcile at pipe close can therefore see this run's holder as
 * alive, keep the record and leave the floor raised. Loop, bounded, until
 * this run's holder is really out of the record (gone, a zombie, or its
 * start time reused); a holder still alive at the bound -- an intentional
 * early pipe close under a live run -- keeps its record and its floor. */
static void ane_floor_watchdog_reconcile(const struct ane_floor_paths *p, int ppid)
{
	const struct timespec step = { 0, 20 * 1000 * 1000 };
	struct ane_floor_state st;
	int i, j;

	for (i = 0; i < 250; i++) {
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
		if (!found || ane_floor_pid_dead(ppid, ticks))
			return;	/* this run is out; whoever is left is alive */
		nanosleep(&step, NULL);
	}
}

/* The watchdog: the parent holds the write end of a pipe for its whole life;
 * the kernel closes it on death (the clean end closes it on purpose), and the
 * child then reconciles under the lock. */
static void ane_floor_watchdog_start(const struct ane_floor_paths *p)
{
	int fds[2];
	pid_t kid;
	pid_t ppid = getpid();	/* read before the fork: the child inherits it */

	if (pipe(fds))
		return;
	kid = fork();
	if (kid < 0) {
		close(fds[0]);
		close(fds[1]);
		return;
	}
	if (kid == 0) {
		struct pollfd pfd;
		int rfd = fds[0];

		close(fds[1]);
		pfd.fd = rfd;
		pfd.events = POLLIN;
		for (;;) {
			pfd.revents = 0;
			if (poll(&pfd, 1, -1) < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))
				break;
		}
		close(rfd);
		ane_floor_watchdog_reconcile(p, (int)ppid);
		_exit(0);
	}
	close(fds[0]);
	ane_floor_pipe_w = fds[1];
}

/* The one restore used by every in-process exit path (atexit and the signal
 * handler): async-signal-safe calls only. */
static void ane_floor_release(const struct ane_floor_paths *p)
{
	struct ane_floor_state st;
	char saved[32], *w = saved;
	int fd, i, live = 0;

	fd = ane_floor_lock_at(p, 0);	/* no mkdir: this runs on the signal path */
	if (fd < 0)
		return;
	if (ane_floor_state_read(p->state, &st) == 0) {
		for (i = 0; i < st.npids; i++) {
			if (st.pids[i] != (int)getpid()) {
				st.pids[live] = st.pids[i];
				st.ticks[live] = st.ticks[i];
				live++;
			}
		}
		st.npids = live;
		if (st.npids == 0) {
			w = ane_floor_num(w, st.saved);
			*w++ = '\n';
			*w = 0;
			ane_floor_write(p->freq, saved);
			unlink(p->state);
		} else {
			ane_floor_state_write(p, &st);
		}
	}
	close(fd);
	if (ane_floor_pipe_w >= 0) {
		close(ane_floor_pipe_w);
		ane_floor_pipe_w = -1;
	}
}

static void ane_floor_signal(int sig)
{
	int again = 0;
	struct sigaction *o;

	o = &ane_floor_old[sig == SIGINT ? 0 : 1];
	if (o->sa_handler == SIG_IGN) {
		/* the run inherited SIG_IGN (nohup, a background job): the
		 * signal is not meant for this process, so put it back and
		 * keep holding the floor */
		sigaction(sig, o, NULL);
		return;
	}
	ane_floor_release(&ane_floor_paths);
	ane_floor_held = 0;
	if (o->sa_handler == SIG_DFL)
		again = 1;
	sigaction(sig, o, NULL);
	if (again)
		raise(sig);
	/* any other previous disposition is re-installed above; its own
	 * semantics apply from there */
}

/* Raise the floor for this run. Returns 1 when this process holds it (and so
 * must let ane_cpu_floor_end run), 0 when the feature does not apply. Never
 * fails the caller. */
static int ane_cpu_floor_begin(void)
{
	struct ane_floor_paths *p = &ane_floor_paths;
	struct ane_floor_state st;
	char buf[128], floor_out[32], *end;
	const char *root, *dir, *env;
	unsigned long long saved, floor, my_ticks;
	int lockfd, raised_fresh = 0;

	/* Opt-in gate: with ANE_CPU_FLOOR unset, zero, or garbage the feature is
	 * off and nothing under sysfs is read or written. */
	env = getenv(ANE_CPU_FLOOR_ENV);
	if (!env || *env < '1' || *env > '9')
		return 0;
	floor = strtoull(env, &end, 10);
	if (end == env || floor == 0 || *end)
		return 0;
	{
		char *w = floor_out;

		w = ane_floor_num(w, floor);
		*w++ = '\n';
		*w = 0;
	}
	if (ane_floor_held)
		return 1;
	root = getenv(ANE_CPU_FLOOR_SYSFS_ENV);
	dir = getenv(ANE_CPU_FLOOR_STATE_ENV);
	if (ane_floor_join(p->freq, sizeof(p->freq),
			   root ? root : "/sys/devices/system/cpu",
			   ANE_CPU_FLOOR_CPU "/cpufreq/scaling_min_freq"))
		return 0;
	if (ane_floor_join(p->compat, sizeof(p->compat),
			   root ? root : "/proc/device-tree", "compatible"))
		return 0;
	if (strlen(dir ? dir : "/run/ane-run") >= sizeof(p->dir))
		return 0;
	snprintf(p->dir, sizeof(p->dir), "%s", dir ? dir : "/run/ane-run");
	if (ane_floor_join(p->state, sizeof(p->state), p->dir, "ane-run.state"))
		return 0;
	if (ane_floor_join(p->lock, sizeof(p->lock), p->dir, "ane-run.lock"))
		return 0;

	/* SoC gate: t6021 in the device tree, else silent no-op (M1 and every
	 * other chip). compatible is a NUL separated list of entries
	 * ("apple,j414c\0apple,t6021\0apple,arm-platform\0"); a strstr on the
	 * whole buffer scans only the first entry, so walk every entry and
	 * require whole-entry equality with "apple,t6021" - a longer entry such
	 * as "apple,t60210" is not a match. */
	{
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
	}
	/* What the file holds now (an orphaned floor from a dead run counts only
	 * after the stale record below has been reconciled). */
	if (ane_floor_read(p->freq, buf, sizeof(buf)) < 0)
		return 0;
	saved = strtoull(buf, &end, 10);
	if (end == buf)
		return 0;
	my_ticks = ane_floor_start_ticks((int)getpid());

	lockfd = ane_floor_lock(p);
	if (lockfd < 0)
		return 0;
	if (ane_floor_state_read(p->state, &st) == 0) {
		ane_floor_prune(&st);
		if (st.npids > 0) {
			/* live holders: join them, keep the floor applied */
			if (st.npids == ANE_CPU_FLOOR_HOLDER_MAX) {
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
			st.npids++;
			ane_floor_write(p->freq, floor_out);
			if (ane_floor_state_write(p, &st)) {
				/* leave the state as the other holders wrote it */
				st.npids--;
				ane_floor_state_write(p, &st);
				close(lockfd);
				return 0;
			}
		} else {
			/* stale: undo the orphaned floor first, then raise fresh */
			{
				char saved_out[32], *w = saved_out;

				w = ane_floor_num(w, st.saved);
				*w++ = '\n';
				*w = 0;
				ane_floor_write(p->freq, saved_out);
			}
			unlink(p->state);
			/* the value to save now is what the file holds after the undo,
			 * not what it held when this run started (that was the orphan) */
			if (ane_floor_read(p->freq, buf, sizeof(buf)) < 0) {
				close(lockfd);
				return 0;
			}
			saved = strtoull(buf, &end, 10);
			if (end == buf) {
				close(lockfd);
				return 0;
			}
			st.saved = saved;
			st.npids = 1;
			st.pids[0] = (int)getpid();
			st.ticks[0] = my_ticks;
			ane_floor_state_write(p, &st);
			if (ane_floor_write(p->freq, floor_out)) {
				unlink(p->state);
				close(lockfd);
				return 0;
			}
			raised_fresh = 1;
		}
	} else {
		/* fresh, no record: the already-at-floor shortcut lives here, so a
		 * holder joining a raised word or reconciling an orphan above is
		 * never refused by it */
		if (saved >= floor) {
			close(lockfd);
			return 0;
		}
		/* record the saved value BEFORE touching the file, so a crash
		 * between the two writes still reconciles */
		st.saved = saved;
		st.npids = 1;
		st.pids[0] = (int)getpid();
		st.ticks[0] = my_ticks;
		if (ane_floor_state_write(p, &st)) {
			close(lockfd);
			return 0;
		}
		if (ane_floor_write(p->freq, floor_out)) {
			/* no write access: silent skip, leave nothing behind */
			unlink(p->state);
			close(lockfd);
			return 0;
		}
		raised_fresh = 1;
	}
	close(lockfd);
	ane_floor_held = 1;

	/* the watchdog covers SIGKILL and crashes; it is forked before the
	 * signal handlers and the atexit hook, so it inherits neither */
	ane_floor_watchdog_start(p);

	atexit(ane_cpu_floor_end);
	{
		struct sigaction act;

		memset(&act, 0, sizeof(act));
		act.sa_handler = ane_floor_signal;
		sigaction(SIGINT, &act, &ane_floor_old[0]);
		sigaction(SIGTERM, &act, &ane_floor_old[1]);
	}

	if (raised_fresh)
		fprintf(stderr, "ane-run: cpu floor %s/%s %llu -> %llu kHz, restored on exit\n",
			ANE_CPU_FLOOR_CPU, "scaling_min_freq", saved, floor);
	else
		fprintf(stderr, "ane-run: cpu floor %s/%s held at %llu kHz, restored on exit\n",
			ANE_CPU_FLOOR_CPU, "scaling_min_freq", floor);
	return 1;
}

static void ane_cpu_floor_end(void)
{
	if (!ane_floor_held)
		return;
	ane_floor_held = 0;
	ane_floor_release(&ane_floor_paths);
}

#endif /* ANE_CPU_FLOOR_H */
