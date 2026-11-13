// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

/* Host unit suite for ane_cpu_floor.h: the state machine over fake sysfs
 * and state trees (no device, no signals). Process-level cases (signals,
 * the watchdog, real ane-run) live in test_cpu_floor.py.
 *
 * usage: ./test_cpu_floor
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ane_cpu_floor.h"

static int checks;
static int fails;

#define EXPECT(c, m) do { \
	checks++; \
	if (!(c)) { \
		printf("FAIL: %s\n", m); \
		fails++; \
	} \
} while (0)

static void ane_floor_test_signal(int sig)
{
	(void)sig;	/* swallow the re-raised signal: proves the release ran */
}

/* pipe/system calls in the forked cases cannot legitimately fail; (void)
 * does not satisfy -Wunused-result under _FORTIFY_SOURCE, so assert them */
static void io_or_die(int ok)
{
	if (!ok)
		_exit(3);
}

/* /proc/device-tree/compatible on the 14-inch M2 Max: NUL separated entries,
 * captured from row m2-cpu-floor-pr411-078's finding. strlen on the buffer
 * sees only the first entry. */
static const char compat_real_m2[] =
	"apple,j414c\0apple,t6021\0apple,arm-platform\0";

static void mkcompat(const char *root, const void *data, size_t len)
{
	char path[1024];
	FILE *f;

	ane_floor_join(path, sizeof(path), root, "compatible");
	f = fopen(path, "w");
	if (!f)
		return;
	fwrite(data, 1, len, f);
	fclose(f);
}

static void mkfreq(const char *root, const char *compat, const char *val)
{
	char path[1024];
	FILE *f;

	mkdir(root, 0755);
	ane_floor_join(path, sizeof(path), root, "cpu4");
	mkdir(path, 0755);
	ane_floor_join(path, sizeof(path), root, "cpu4/cpufreq");
	mkdir(path, 0755);
	ane_floor_join(path, sizeof(path), root, "cpu4/cpufreq/scaling_min_freq");
	unlink(path);	/* a fifo left by the off-by-default case would block the open */
	f = fopen(path, "w");
	if (!f)
		return;
	fputs(val, f);
	fclose(f);
	ane_floor_join(path, sizeof(path), root, "compatible");
	f = fopen(path, "w");
	fputs(compat, f);
	fclose(f);
}

static char *freq_of(const char *root)
{
	static char buf[128];
	char path[1024];
	FILE *f;

	ane_floor_join(path, sizeof(path), root, "cpu4/cpufreq/scaling_min_freq");
	f = fopen(path, "r");
	if (!f)
		return "<missing>";
	if (!fgets(buf, sizeof(buf), f))
		strcpy(buf, "<empty>");
	fclose(f);
	{
		size_t n = strlen(buf);
		while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
			buf[--n] = 0;
	}
	return buf;
}

static char state_buf[4096];

static const char *state_of(const char *var)
{
	static char path[1024];
	FILE *f;

	ane_floor_join(path, sizeof(path), var, "ane-run.state");
	f = fopen(path, "r");
	if (!f)
		return "<absent>";
	{
		size_t n = fread(state_buf, 1, sizeof(state_buf) - 1, f);
		state_buf[n] = 0;
	}
	fclose(f);
	return state_buf;
}

int main(void)
{
	char base[1024], sysfs[1024], var[1024];
	const char *saved;

	snprintf(base, sizeof(base), "/tmp/ane-floor-test.%d", (int)getpid());
	mkdir(base, 0755);
	ane_floor_join(sysfs, sizeof(sysfs), base, "sys");
	ane_floor_join(var, sizeof(var), base, "var");
	setenv(ANE_CPU_FLOOR_SYSFS_ENV, sysfs, 1);
	setenv(ANE_CPU_FLOOR_STATE_ENV, var, 1);

	/* 0: the floor is opt-in. With ANE_CPU_FLOOR unset the code must not read
	 * or write sysfs at all: the freq file is a FIFO with no writer, so any
	 * read would block the suite until the alarm fires and kills it. */
	{
		char path[1024], probe[32];

		unsetenv(ANE_CPU_FLOOR_ENV);
		unsetenv(ANE_CPU_FLOOR_STATE_ENV);
		mkdir(sysfs, 0755);
		mkfreq(sysfs, "apple,t6021", "placeholder");
		ane_floor_join(path, sizeof(path), sysfs, "cpu4/cpufreq/scaling_min_freq");
		unlink(path);
		if (mkfifo(path, 0644))
			printf("FAIL: cannot make the fifo (%s)\n", strerror(errno)), fails++, checks++;
		{
			struct stat st2;

			EXPECT(stat(path, &st2) == 0 && S_ISFIFO(st2.st_mode), "sanity: the freq path is a fifo");
		}
		(void)probe;
		alarm(10);
		EXPECT(ane_cpu_floor_begin() == 0, "no ANE_CPU_FLOOR: begin skips");
		alarm(0);
		EXPECT(strstr(state_of(var), "<absent>") != NULL, "no ANE_CPU_FLOOR: no state file");
		ane_cpu_floor_end();
		/* garbage and zero opt-ins are the same no-op */
		setenv(ANE_CPU_FLOOR_ENV, "nonsense", 1);
		EXPECT(ane_cpu_floor_begin() == 0, "garbage ANE_CPU_FLOOR: begin skips");
		setenv(ANE_CPU_FLOOR_ENV, "0", 1);
		EXPECT(ane_cpu_floor_begin() == 0, "zero ANE_CPU_FLOOR: begin skips");
		setenv(ANE_CPU_FLOOR_STATE_ENV, var, 1);
		setenv(ANE_CPU_FLOOR_ENV, "1000000", 1);
		mkfreq(sysfs, "apple,t6021", "702000\n");
		EXPECT(ane_cpu_floor_begin() == 1, "a custom floor value is honoured");
		EXPECT(strstr(freq_of(sysfs), "1000000") != NULL, "the floor comes from the env, not a constant");
		ane_cpu_floor_end();
		EXPECT(strcmp(freq_of(sysfs), "702000") == 0, "the custom cycle restores too");
		/* trailing junk after the digits is not a number either */
		setenv(ANE_CPU_FLOOR_ENV, "1968000kHz", 1);
		EXPECT(ane_cpu_floor_begin() == 0, "trailing junk (kHz): begin skips");
		setenv(ANE_CPU_FLOOR_ENV, "12x", 1);
		EXPECT(ane_cpu_floor_begin() == 0, "trailing junk (12x): begin skips");
		setenv(ANE_CPU_FLOOR_ENV, "1e6", 1);
		EXPECT(ane_cpu_floor_begin() == 0, "exponent junk (1e6): begin skips");
		setenv(ANE_CPU_FLOOR_ENV, ANE_CPU_FLOOR_KHZ_DEFAULT, 1);
	}

	/* 1: normal restore -- raise, then end puts the SAVED value back */
	mkdir(var, 0755);
	mkfreq(sysfs, "apple,t6021", "702000\n");
	mkcompat(sysfs, compat_real_m2, sizeof(compat_real_m2));
	EXPECT(ane_cpu_floor_begin() == 1, "t6021 with policy4: begin participates");
	EXPECT(strcmp(freq_of(sysfs), "1968000") == 0, "begin writes the floor");
	EXPECT(strstr(state_of(var), "saved 702000") != NULL, "state records the saved value");
	ane_cpu_floor_end();
	EXPECT(strcmp(freq_of(sysfs), "702000") == 0, "end restores the saved value");
	EXPECT(strstr(state_of(var), "<absent>") != NULL, "end removes the state file");

	/* 2: the saved value is whatever was there, never a constant */
	mkfreq(sysfs, "apple,t6021", "553000\n");
	EXPECT(ane_cpu_floor_begin() == 1, "second cycle participates");
	ane_cpu_floor_end();
	EXPECT(strcmp(freq_of(sysfs), "553000") == 0, "restore is not hardcoded");

	/* 3: end twice is quiet */
	ane_cpu_floor_end();
	EXPECT(strcmp(freq_of(sysfs), "553000") == 0, "a second end writes nothing");

	/* 4: no t6021 in the device tree (M1, t8103): silent no-op */
	mkfreq(sysfs, "apple,j293\napple,t8103\n", "702000\n");
	EXPECT(ane_cpu_floor_begin() == 0, "t8103: begin skips");
	EXPECT(strcmp(freq_of(sysfs), "702000") == 0, "t8103: nothing written");
	EXPECT(strstr(state_of(var), "<absent>") != NULL, "t8103: no state file");

	/* 5: no policy4: silent no-op */
	mkfreq(sysfs, "apple,t6021", "");
	{
		char path[1024];
		ane_floor_join(path, sizeof(path), sysfs, "cpu4/cpufreq/scaling_min_freq");
		unlink(path);
	}
	EXPECT(ane_cpu_floor_begin() == 0, "no policy4: begin skips");
	EXPECT(strstr(state_of(var), "<absent>") != NULL, "no policy4: no state file");

	/* 6: saved value unreadable (garbage): silent no-op */
	mkfreq(sysfs, "apple,t6021", "nonsense\n");
	EXPECT(ane_cpu_floor_begin() == 0, "unreadable saved value: begin skips");

	/* 7: already at or above the floor: nothing to do */
	mkfreq(sysfs, "apple,t6021", "1968000\n");
	EXPECT(ane_cpu_floor_begin() == 0, "floor already applied: begin skips");
	mkfreq(sysfs, "apple,t6021", "3264000\n");
	EXPECT(ane_cpu_floor_begin() == 0, "above the floor: begin skips");

	/* 8: write refused (read-only sysfs file): silent skip, no state */
	mkfreq(sysfs, "apple,t6021", "702000\n");
	{
		char path[1024];
		ane_floor_join(path, sizeof(path), sysfs, "cpu4/cpufreq/scaling_min_freq");
		(void)chmod(path, 0444);
		if (geteuid() != 0) {
			EXPECT(ane_cpu_floor_begin() == 0, "unwritable sysfs: begin skips");
			EXPECT(strstr(state_of(var), "<absent>") != NULL, "unwritable sysfs: no state");
		}
		(void)chmod(path, 0644);
	}

	/* 9: two holders -- the floor outlives the first to leave, the last restores */
	mkfreq(sysfs, "apple,t6021", "702000\n");
	EXPECT(ane_cpu_floor_begin() == 1, "first holder raises");
	{
		pid_t kid = fork();

		if (kid == 0) {
			ane_cpu_floor_forked_child();
			ane_cpu_floor_begin();
			ane_cpu_floor_end();
			_exit(0);
		}
		{
			int st;
			waitpid(kid, &st, 0);
			EXPECT(WIFEXITED(st) && WEXITSTATUS(st) == 0, "second holder exited clean");
		}
		EXPECT(strstr(freq_of(sysfs), "1968000") != NULL, "floor outlives the first leaver");
		EXPECT(strstr(state_of(var), "saved 702000") != NULL, "state keeps the saved value");
		ane_cpu_floor_end();
		EXPECT(strcmp(freq_of(sysfs), "702000") == 0, "last holder restores");
		EXPECT(strstr(state_of(var), "<absent>") != NULL, "state gone after the last holder");
	}

	/* 10: stale state (holder pid is dead) is reconciled by the next begin */
	mkfreq(sysfs, "apple,t6021", "1968000\n");
	{
		char path[1024];
		FILE *f;
		ane_floor_join(path, sizeof(path), var, "ane-run.state");
		f = fopen(path, "w");
		fputs("saved 702000\npid 4000000\n", f);	/* 4000000: no such process */
		fclose(f);
	}
	EXPECT(ane_cpu_floor_begin() == 1, "stale state: begin reconciles and raises");
	saved = freq_of(sysfs);
	EXPECT(strcmp(saved, "1968000") == 0, "after reconcile the floor is applied fresh");
	EXPECT(strstr(state_of(var), "pid 4000000") == NULL, "dead holder dropped from the state");
	ane_cpu_floor_end();
	EXPECT(strcmp(freq_of(sysfs), "702000") == 0, "the reconciled cycle restores too");

	/* 10b: a stranger's reuse of a live pid (start ticks no longer match)
	 * is dead: the prune drops it even though kill() would still answer */
	mkfreq(sysfs, "apple,t6021", "702000\n");
	{
		char path[1024];
		FILE *f;

		ane_floor_join(path, sizeof(path), var, "ane-run.state");
		f = fopen(path, "w");
		fprintf(f, "saved 702000\npid %d 1\n", (int)getpid());
		fclose(f);
	}
	EXPECT(ane_cpu_floor_begin() == 1, "a reused stranger pid is dropped and the run raises");
	EXPECT(strstr(state_of(var), " 1\n") == NULL, "the stranger's stale record is pruned");
	ane_cpu_floor_end();
	EXPECT(strstr(state_of(var), "<absent>") != NULL, "the reconciled run clears the record");

	/* 11: an end under a live foreign holder leaves the floor to that holder */
	mkfreq(sysfs, "apple,t6021", "702000\n");
	{
		pid_t kid = fork();

		if (kid == 0) {
			ane_cpu_floor_forked_child();
			ane_cpu_floor_begin();
			sleep(1);
			ane_cpu_floor_end();
			_exit(0);
		}
		usleep(200 * 1000);
		EXPECT(ane_cpu_floor_begin() == 1, "join under a live holder");
		ane_cpu_floor_end();
		EXPECT(strstr(freq_of(sysfs), "1968000") != NULL, "live holder keeps the floor");
		{
			int st;
			waitpid(kid, &st, 0);
			EXPECT(WIFEXITED(st) && WEXITSTATUS(st) == 0, "foreign holder exited");
		}
		EXPECT(strcmp(freq_of(sysfs), "702000") == 0, "foreign holder restored at its end");
	}

	/* 12: a full holder table -- the run sits out instead of holding
	 * without a record (a later failed write there would decrement
	 * somebody else's entry) */
	mkfreq(sysfs, "apple,t6021", "702000\n");
	{
		char path[1024];
		FILE *f;
		unsigned long long my = ane_floor_start_ticks((int)getpid());
		int i;

		ane_floor_join(path, sizeof(path), var, "ane-run.state");
		f = fopen(path, "w");
		fprintf(f, "saved 702000\n");
		for (i = 0; i < ANE_CPU_FLOOR_HOLDER_MAX; i++)
			fprintf(f, "pid %d %llu\n", (int)getpid(), my);
		fclose(f);
		EXPECT(ane_cpu_floor_begin() == 0, "full holder table: begin sits out");
		EXPECT(strstr(freq_of(sysfs), "702000") != NULL, "full table: the floor stays with the holders");
		EXPECT(strstr(state_of(var), "pid ") != NULL, "full table: the record survives untouched");
		(void)unlink(path);
	}

	/* 13: the saved value is read under the lock: an administrator value
	 * that lands in the file while a begin waits for the lock must not be
	 * clobbered by a restore of a stale value read before the lock */
	mkfreq(sysfs, "apple,t6021", "702000\n");
	{
		pid_t holder, kid;
		int st, syncpipe[2], gopipe[2];
		char c;
		struct timespec settle = { 0, 500 * 1000 * 1000 };

		EXPECT(pipe(syncpipe) == 0 && pipe(gopipe) == 0, "case 13 pipes open");
		holder = fork();
		if (holder == 0) {
			int lfd2;
			char lpath[1024];

			ane_floor_join(lpath, sizeof(lpath), var, "ane-run.lock");
			lfd2 = open(lpath, O_RDWR | O_CREAT, 0644);
			flock(lfd2, LOCK_EX);
			io_or_die(write(syncpipe[1], "L", 1) == 1);
			io_or_die(read(gopipe[0], &c, 1) == 1);
			/* the administrator change lands while the begin waits */
			mkfreq(sysfs, "apple,t6021", "553000\n");
			close(lfd2);	/* the waiting begin proceeds */
			_exit(0);
		}
		io_or_die(read(syncpipe[0], &c, 1) == 1);	/* the lock is held */
		kid = fork();
		if (kid == 0) {
			ane_cpu_floor_forked_child();
			ane_cpu_floor_begin();
			ane_cpu_floor_end();
			_exit(0);
		}
		nanosleep(&settle, NULL);	/* let the begin block on the lock */
		io_or_die(write(gopipe[1], "G", 1) == 1);
		waitpid(holder, &st, 0);
		waitpid(kid, &st, 0);
		close(syncpipe[0]);
		close(syncpipe[1]);
		close(gopipe[0]);
		close(gopipe[1]);
		EXPECT(strcmp(freq_of(sysfs), "553000") == 0,
			"an admin change landing during begin is not clobbered");
	}

	/* 10: the real compatible bytes are NUL separated; the gate must scan
	 * every entry, so t6021 in the SECOND entry still engages the floor,
	 * while a t60210 entry (t6021 as a substring) must not match. */
	{
		static const char compat_t60210[] = "apple,t60210\0apple,arm-platform\0";

		mkfreq(sysfs, "", "702000\n");
		mkcompat(sysfs, compat_real_m2, sizeof(compat_real_m2));
		EXPECT(ane_cpu_floor_begin() == 1, "NUL separated compatible: the second entry matches");
		EXPECT(strstr(freq_of(sysfs), "1968000") != NULL, "the floor engaged on the real bytes");
		ane_cpu_floor_end();
		EXPECT(strcmp(freq_of(sysfs), "702000") == 0, "and restored again");

		mkfreq(sysfs, "", "702000\n");
		mkcompat(sysfs, compat_t60210, sizeof(compat_t60210));
		EXPECT(ane_cpu_floor_begin() == 0, "t6021 as a substring of t60210 does not match");
		EXPECT(strstr(freq_of(sysfs), "702000") != NULL, "the near-miss entry leaves sysfs alone");

		/* the list's last entry without its trailing NUL is still a
		 * whole entry: the read NUL-terminates the buffer */
		mkfreq(sysfs, "", "702000\n");
		mkcompat(sysfs, "apple,j414c\0apple,t6021", 23);
		EXPECT(ane_cpu_floor_begin() == 1, "compatible without the trailing NUL matches");
		EXPECT(strstr(freq_of(sysfs), "1968000") != NULL, "the unterminated list engaged the floor");
		ane_cpu_floor_end();
		EXPECT(strcmp(freq_of(sysfs), "702000") == 0, "and restored after the unterminated list");

		/* an empty compatible file names no SoC: silent no-op */
		mkfreq(sysfs, "", "702000\n");
		mkcompat(sysfs, "", 0);
		EXPECT(ane_cpu_floor_begin() == 0, "empty compatible: begin skips");
		EXPECT(strstr(state_of(var), "<absent>") != NULL, "empty compatible: no state file");
	}

	/* an inherited SIG_IGN (nohup, a background job) is not meant for this
	 * run: the handler puts it back and the floor survives the delivery */
	{
		struct sigaction ign, got;

		memset(&ign, 0, sizeof(ign));
		ign.sa_handler = SIG_IGN;
		sigaction(SIGTERM, &ign, NULL);
		mkfreq(sysfs, "apple,t6021", "702000\n");
		EXPECT(ane_cpu_floor_begin() == 1, "raise under an inherited SIG_IGN");
		raise(SIGTERM);
		EXPECT(sigaction(SIGTERM, NULL, &got) == 0 && got.sa_handler == SIG_IGN,
			"the handler reinstated SIG_IGN");
		EXPECT(strstr(freq_of(sysfs), "1968000") != NULL,
			"SIG_IGN delivery leaves the floor raised");
		EXPECT(strstr(state_of(var), "saved 702000") != NULL,
			"SIG_IGN delivery leaves the state");
		ane_cpu_floor_end();
		EXPECT(strcmp(freq_of(sysfs), "702000") == 0, "the floor restores after SIG_IGN");
		EXPECT(strstr(state_of(var), "<absent>") != NULL, "state gone after SIG_IGN");
	}

	/* 9: the signal handler itself must restore before it hands the signal
	 * back: a captured SIGTERM proves the release happened in the dying
	 * process, not later in the watchdog. */
	{
		struct sigaction cap, got;

		memset(&cap, 0, sizeof(cap));
		cap.sa_handler = ane_floor_test_signal;
		sigaction(SIGTERM, &cap, NULL);
		mkfreq(sysfs, "apple,t6021", "702000\n");
		EXPECT(ane_cpu_floor_begin() == 1, "raise before the captured signal");
		EXPECT(strstr(freq_of(sysfs), "1968000") != NULL, "floor applied before the signal");
		raise(SIGTERM);
		EXPECT(sigaction(SIGTERM, NULL, &got) == 0 && got.sa_handler == ane_floor_test_signal,
			"the handler reinstated the captured disposition");
		EXPECT(strcmp(freq_of(sysfs), "702000") == 0,
			"the signal handler restored in this process");
		EXPECT(strstr(state_of(var), "<absent>") != NULL,
			"the signal handler removed the state");
	}

	snprintf(base, sizeof(base), "rm -rf /tmp/ane-floor-test.%d", (int)getpid());
	io_or_die(system(base) == 0);

	printf("RESULT: %d checks, %d failures\n", checks, fails);
	return fails != 0;
}
