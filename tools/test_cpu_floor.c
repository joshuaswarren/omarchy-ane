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

	/* 1: normal restore -- raise, then end puts the SAVED value back */
	mkdir(var, 0755);
	mkfreq(sysfs, "apple,j414s\napple,t6021\n", "702000\n");
	EXPECT(ane_cpu_floor_begin() == 1, "t6021 with policy4: begin participates");
	EXPECT(strcmp(freq_of(sysfs), "1968000") == 0, "begin writes the floor");
	EXPECT(strstr(state_of(var), "saved 702000") != NULL, "state records the saved value");
	ane_cpu_floor_end();
	EXPECT(strcmp(freq_of(sysfs), "702000") == 0, "end restores the saved value");
	EXPECT(strstr(state_of(var), "<absent>") != NULL, "end removes the state file");

	/* 2: the saved value is whatever was there, never a constant */
	mkfreq(sysfs, "apple,t6021\n", "553000\n");
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
	mkfreq(sysfs, "apple,t6021\n", "");
	{
		char path[1024];
		ane_floor_join(path, sizeof(path), sysfs, "cpu4/cpufreq/scaling_min_freq");
		unlink(path);
	}
	EXPECT(ane_cpu_floor_begin() == 0, "no policy4: begin skips");
	EXPECT(strstr(state_of(var), "<absent>") != NULL, "no policy4: no state file");

	/* 6: saved value unreadable (garbage): silent no-op */
	mkfreq(sysfs, "apple,t6021\n", "nonsense\n");
	EXPECT(ane_cpu_floor_begin() == 0, "unreadable saved value: begin skips");

	/* 7: already at or above the floor: nothing to do */
	mkfreq(sysfs, "apple,t6021\n", "1968000\n");
	EXPECT(ane_cpu_floor_begin() == 0, "floor already applied: begin skips");
	mkfreq(sysfs, "apple,t6021\n", "3264000\n");
	EXPECT(ane_cpu_floor_begin() == 0, "above the floor: begin skips");

	/* 8: write refused (read-only sysfs file): silent skip, no state */
	mkfreq(sysfs, "apple,t6021\n", "702000\n");
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
	mkfreq(sysfs, "apple,t6021\n", "702000\n");
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
	mkfreq(sysfs, "apple,t6021\n", "1968000\n");
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

	/* 11: an end under a live foreign holder leaves the floor to that holder */
	mkfreq(sysfs, "apple,t6021\n", "702000\n");
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

	snprintf(base, sizeof(base), "rm -rf /tmp/ane-floor-test.%d", (int)getpid());
	(void)system(base);

	printf("RESULT: %d checks, %d failures\n", checks, fails);
	return fails != 0;
}
