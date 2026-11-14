/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Joshua Warren */
/* Host test of the CPU latency hold in libane/ane_latency.h. No device: ANE_CPU_LATENCY_DEV points at a plain file. */
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ane_latency.h"

static int bad;
static char path[] = "/tmp/test_ane_latency.XXXXXX";

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
	size_t got = f ? fread(&word, 1, sizeof(word) + 1, f) : 0;

	if (f) {
		fclose(f);
	}
	return got == sizeof(word) && word == 0;
}

static int wait_closed(long limit_ms)
{
	long waited;

	for (waited = 0; waited < limit_ms; waited += 5) {
		if (open_fds() == 0) {
			return 1;
		}
		sleep_ms(5);
	}
	return 0;
}

int main(int argc, char **argv)
{
	int fd = mkstemp(path);
	int i, steady = 1;
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
		ane_latency_touch();
		ane_latency_touch();
		expect("unopenable file: dispatch continues, hold disabled", ane_lat.disabled && ane_lat.fd < 0);
		return bad;
	}

	setenv("ANE_CPU_LATENCY_HOLD_MS", "30", 1);
	expect("no hold before the first dispatch", open_fds() == 0);
	ane_latency_touch();
	expect("first dispatch opens the file", open_fds() == 1);
	expect("first dispatch writes a 32-bit zero", file_is_zero_word());

	for (i = 0; i < 20; i++) {
		sleep_ms(10);
		ane_latency_touch();
		steady &= open_fds() == 1;
	}
	expect("dispatches 10 ms apart keep one descriptor open the whole time", steady);

	expect("the hold drops after the idle time", wait_closed(1000));
	expect("the watcher flag matches", __atomic_load_n(&ane_lat.held, __ATOMIC_ACQUIRE) == 0);

	reset = fopen(path, "w");
	fputs("x", reset);
	fclose(reset);
	ane_latency_touch();
	expect("the next dispatch holds again", open_fds() == 1 && file_is_zero_word());
	expect("and it drops again", wait_closed(1000));

	unlink(path);
	return bad;
}
