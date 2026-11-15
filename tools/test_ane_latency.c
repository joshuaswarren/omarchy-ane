/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Joshua Warren */
/* Host test of the CPU latency hold in libane/ane_latency.h. No device: ANE_CPU_LATENCY_DEV points at a plain file. */
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ane_latency.h"

static int bad;
static char path[] = "/tmp/test_ane_latency.XXXXXX";
static char errpath[] = "/tmp/test_ane_latency_err.XXXXXX";

static void expect(const char *name, int ok)
{
	printf("%s %s\n", ok ? "PASS" : "FAIL", name);
	bad |= !ok;
}

static void sleep_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

static long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* CPU time of the whole process: the watcher must sleep between dispatches, not spin. */
static long cpu_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Descriptors of this process that point at the fake latency file. */
static int open_fds(void)
{
	DIR *d = opendir("/proc/self/fd");
	struct dirent *e;
	int n = 0;

	while ((e = readdir(d))) {
		char link[300], target[PATH_MAX];
		ssize_t len;

		snprintf(link, sizeof(link), "/proc/self/fd/%s", e->d_name);
		len = readlink(link, target, sizeof(target) - 1);
		if (len > 0) {
			target[len] = 0;
			n += strcmp(target, path) == 0;
		}
	}
	closedir(d);
	return n;
}

static int file_is_zero_word(void)
{
	int32_t word = 7;
	FILE *f = fopen(path, "rb");
	size_t got = f ? fread(&word, 1, sizeof(word), f) : 0;

	if (f) {
		fclose(f);
	}
	return got == sizeof(word) && word == 0;
}

/* 1 when the captured stderr file is exactly one line naming the hold being off. */
static int stderr_one_hold_line(void)
{
	char buf[300];
	FILE *f = fopen(errpath, "rb");
	size_t got = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
	char *nl;

	if (f) {
		fclose(f);
	}
	if (!got || got == sizeof(buf) - 1) {
		return 0;
	}
	buf[got] = 0;
	nl = strchr(buf, '\n');
	return nl && nl[1] == 0 && strncmp(buf, "LIBANE:", 7) == 0 &&
	       strstr(buf, "cpu latency hold off") != NULL;
}

/* Milliseconds until the hold drops, or -1 once limit_ms passed with the file still open. */
static long wait_closed_ms(long limit_ms)
{
	long t0 = now_ms();

	for (;;) {
		long at = now_ms() - t0;

		if (open_fds() == 0) {
			return at;
		}
		if (at >= limit_ms) {
			return -1;
		}
		sleep_ms(5);
	}
}

int main(int argc, char **argv)
{
	int fd = mkstemp(path);
	int i, steady = 1, cycles = 0;
	long dropped, c0;
	pid_t pid;
	FILE *reset;

	if (fd < 0) {
		return 2;
	}
	if (write(fd, "x", 1) != 1) {
		return 2;
	}
	close(fd);
	setenv("ANE_CPU_LATENCY_DEV", argc > 1 && !strcmp(argv[1], "missing") ? "/nonexistent/latency" : path, 1);

	if (argc > 1 && !strcmp(argv[1], "off")) {
		setenv("ANE_CPU_LATENCY_HOLD_MS", "0", 1);
		ane_latency_touch();
		expect("hold off: no descriptor opened", open_fds() == 0);
		expect("hold off: file untouched", !file_is_zero_word());
		unlink(path);
		return bad;
	}
	if (argc > 1 && !strcmp(argv[1], "missing")) {
		int errfd = dup(STDERR_FILENO);
		int cap = mkstemp(errpath);
		struct stat st;

		fflush(stderr);
		dup2(cap, STDERR_FILENO);
		ane_latency_touch();
		ane_latency_touch();
		fflush(stderr);
		dup2(errfd, STDERR_FILENO);
		close(errfd);
		close(cap);
		expect("unopenable file: dispatch continues, hold disabled", ane_lat.disabled && ane_lat.fd < 0);
		expect("default-on failure stays silent on stderr", stat(errpath, &st) == 0 && st.st_size == 0);
		unlink(errpath);
		return bad;
	}
	if (argc > 1 && !strcmp(argv[1], "default")) {
		unsetenv("ANE_CPU_LATENCY_HOLD_MS");
		ane_latency_touch();
		sleep_ms(25);
		expect("default hold: open at 25 ms", open_fds() == 1);
		expect("default hold: dropped by 500 ms", wait_closed_ms(500) >= 0);
		setenv("ANE_CPU_LATENCY_HOLD_MS", "banana", 1);
		ane_latency_touch();
		expect("garbage parses as the default, not as off", open_fds() == 1);
		unlink(path);
		return bad;
	}

	if (argc > 1 && !strcmp(argv[1], "explicit")) {
		int errfd = dup(STDERR_FILENO);
		int cap = mkstemp(errpath);

		setenv("ANE_CPU_LATENCY_DEV", "/nonexistent/latency", 1);
		setenv("ANE_CPU_LATENCY_HOLD_MS", "30", 1);
		fflush(stderr);
		dup2(cap, STDERR_FILENO);
		ane_latency_touch();
		ane_latency_touch();
		fflush(stderr);
		dup2(errfd, STDERR_FILENO);
		close(errfd);
		close(cap);
		expect("explicit env: dispatch continues, hold disabled", ane_lat.disabled && ane_lat.fd < 0);
		expect("explicit env: exactly one stderr line naming the hold", stderr_one_hold_line());
		unlink(errpath);
		return bad;
	}
	if (argc > 1 && !strcmp(argv[1], "clamp")) {
		/* 18446744073710 ms * 1e6 wraps uint64 to about 0.45 ms: without the day clamp the hold
		   would drop within a few ms. Clamped it is one day, so the hold must still be open
		   after 200 ms and the process must exit cleanly. */
		setenv("ANE_CPU_LATENCY_HOLD_MS", "18446744073710", 1);
		ane_latency_touch();
		sleep_ms(200);
		expect("day clamp: the wrapped value did not become a tiny idle time", open_fds() == 1);
		return bad;
	}

	setenv("ANE_CPU_LATENCY_HOLD_MS", "30", 1);
	expect("no hold before the first dispatch", open_fds() == 0);
	ane_latency_touch();
	expect("first dispatch opens the file", open_fds() == 1);
	expect("first dispatch writes a 32-bit zero", file_is_zero_word());

	/* Sample both sides of each dispatch: a drop between the dispatches reopens, and the cycle count sees it. */
	for (i = 0; i < 20; i++) {
		int before, after;

		sleep_ms(10);
		before = open_fds();
		ane_latency_touch();
		after = open_fds();
		steady &= before == 1 && after == 1;
		cycles += before == 0 && after == 1;
	}
	expect("dispatches 10 ms apart keep one descriptor open the whole time", steady);
	expect("no close/reopen cycle between the dispatches", cycles == 0);

	c0 = cpu_ms();
	dropped = wait_closed_ms(1000);
	expect("the hold drops after the idle time", dropped >= 0);
	expect("the watcher sleeps while waiting, it does not spin", cpu_ms() - c0 <= 15);
	/* Shorter than the idle means it dropped mid-stream; longer than 2x means the timer is wrong (30 ms idle: 24..60). */
	expect("the drop lands at 0.8x..2x the idle time", dropped >= 24 && dropped <= 60);
	expect("the watcher flag matches", __atomic_load_n(&ane_lat.held, __ATOMIC_ACQUIRE) == 0);

	reset = fopen(path, "w");
	fputs("x", reset);
	fclose(reset);
	ane_latency_touch();
	expect("the next dispatch holds again", open_fds() == 1 && file_is_zero_word());

	/* A touch while the hold is open returns early and never re-reads the env, so drop the
	   30 ms hold here before the fork section raises the parent's idle time to 300 ms. */
	expect("and it drops again", wait_closed_ms(1000) >= 0);

	/* The parent holds for 300 ms: longer than the child's own 30 ms drop plus its wait bound,
	   so the child's lifetime cannot expire the parent's hold and the next check stays exact. */
	setenv("ANE_CPU_LATENCY_HOLD_MS", "300", 1);
	ane_latency_touch();
	pid = fork();
	if (pid == 0) {
		int32_t word = 7;
		int child_bad = open_fds() != 0;
		int rf = open(path, O_RDONLY);

		/* The child picks its own idle time again. */
		setenv("ANE_CPU_LATENCY_HOLD_MS", "30", 1);
		ane_latency_touch();
		child_bad |= rf < 0 || read(rf, &word, sizeof(word)) != sizeof(word) || word != 0;
		if (rf >= 0) {
			close(rf);
		}
		child_bad |= open_fds() != 1;
		/* The rebuilt hold is the child's own: no watcher survives the fork, so the child must drop
		   it with no further dispatch (idle 30 ms, bound idle plus slack), or the hold leaks for
		   the child's whole life. Deleting the watcher_running reset in ane_latency_child fails
		   exactly here: the child has no watcher and never drops. */
		child_bad |= wait_closed_ms(150) < 0;
		_exit(child_bad ? 1 : 0);
	}
	{
		int st = -1;

		expect("fork: the child drops the inherited hold, rebuilds its own and drops it too",
		       pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
	}
	expect("fork: the parent's hold is untouched", open_fds() == 1);

	expect("and the parent's hold still drops after the fork", wait_closed_ms(1000) >= 0);

	/* The 30 ms-scale spin check above cannot see busy timedwait wakeups: a watcher whose
	   deadline remaining is mutated to 1 ns burns about 31 ms of CPU per 300 ms hold, which
	   slips under that 15 ms bound. One long hold (the idle time is still the fork section's
	   300 ms) with no dispatch and no fd polling during the window (the poll itself costs
	   CPU): the watcher must stay under 3 ms of process CPU. */
	ane_latency_touch();
	expect("long hold: open right after the dispatch", open_fds() == 1);
	c0 = cpu_ms();
	sleep_ms(400);
	dropped = cpu_ms() - c0;
	expect("long hold: dropped with no further dispatch",
	       open_fds() == 0 && __atomic_load_n(&ane_lat.held, __ATOMIC_ACQUIRE) == 0);
	expect("long hold: the watcher burned under 3 ms of CPU", dropped <= 3);

	unlink(path);
	return bad;
}
