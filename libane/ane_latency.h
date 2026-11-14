/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Joshua Warren */
/*
 * CPU wake-latency hold for the dispatching process.
 *
 * On a 14-inch M2 Max an ANE call that finds every CPU in the deep idle state (cpuidle state1, CPU PD) is slow: with no other load the
 * linked ane-run program had 0.285 of its calls slow, and a process holding /dev/cpu_dma_latency at 0 cut that to 0.017 (a full
 * spinner on a core: 0.004). The hold is a request, not a busy loop: the kernel keeps the CPUs out of state1 while the file stays open.
 *
 * ane_latency_touch() is called once per dispatch. It opens the file and writes a 32-bit 0 when no hold is active, and stamps the
 * time of the dispatch. One watcher thread closes the file when no dispatch has happened for the idle time, so a process that
 * stops dispatching gives the CPUs back their deep idle state. The process exit closes the file too.
 *
 * ANE_CPU_LATENCY_HOLD_MS: idle time in milliseconds before the hold is dropped (default 50). 0 turns the hold off.
 * ANE_CPU_LATENCY_DEV: path of the latency file (default /dev/cpu_dma_latency); the host test points it at a plain file.
 * A file that cannot be opened turns the hold off for the rest of the process with one message on stderr.
 */
#ifndef ANE_LATENCY_H
#define ANE_LATENCY_H

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ANE_LATENCY_DEFAULT_MS 50

struct ane_latency {
	pthread_mutex_t lock;
	pthread_cond_t wake;
	int fd;			/* -1 when no hold is active */
	int held;		/* mirrors fd >= 0 for the lock-free check */
	int disabled;
	int watcher_running;
	uint64_t last_ns;	/* time of the latest dispatch */
	uint64_t idle_ns;
};

static struct ane_latency ane_lat = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
	.wake = PTHREAD_COND_INITIALIZER,
	.fd = -1,
};

static inline uint64_t ane_latency_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static inline void ane_latency_deadline(struct timespec *ts, uint64_t ns)
{
	clock_gettime(CLOCK_REALTIME, ts);
	ns += ts->tv_nsec;
	ts->tv_sec += ns / 1000000000ull;
	ts->tv_nsec = ns % 1000000000ull;
}

/* Waits for the idle time to pass after the latest dispatch, closes the file, then sleeps until the next dispatch opens it. */
static inline void *ane_latency_watcher(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&ane_lat.lock);
	for (;;) {
		uint64_t idle;
		struct timespec ts;

		if (ane_lat.fd < 0) {
			pthread_cond_wait(&ane_lat.wake, &ane_lat.lock);
			continue;
		}
		idle = ane_latency_now() - __atomic_load_n(&ane_lat.last_ns, __ATOMIC_RELAXED);
		if (idle >= ane_lat.idle_ns) {
			close(ane_lat.fd);
			ane_lat.fd = -1;
			__atomic_store_n(&ane_lat.held, 0, __ATOMIC_RELEASE);
			continue;
		}
		ane_latency_deadline(&ts, ane_lat.idle_ns - idle);
		pthread_cond_timedwait(&ane_lat.wake, &ane_lat.lock, &ts);
	}
	return NULL;
}

static inline void ane_latency_open(void)
{
	const char *dev = getenv("ANE_CPU_LATENCY_DEV");
	const char *ms = getenv("ANE_CPU_LATENCY_HOLD_MS");
	int32_t zero = 0;
	long hold = ms ? strtol(ms, NULL, 10) : ANE_LATENCY_DEFAULT_MS;
	pthread_t tid;
	int fd;

	if (hold <= 0) {
		ane_lat.disabled = 1;
		return;
	}
	ane_lat.idle_ns = (uint64_t)hold * 1000000ull;
	fd = open(dev ? dev : "/dev/cpu_dma_latency", O_WRONLY | O_CLOEXEC);
	if (fd < 0 || write(fd, &zero, sizeof(zero)) != sizeof(zero)) {
		fprintf(stderr, "LIBANE: cpu latency hold off: %s\n", strerror(errno));
		if (fd >= 0) {
			close(fd);
		}
		ane_lat.disabled = 1;
		return;
	}
	if (!ane_lat.watcher_running) {
		if (pthread_create(&tid, NULL, ane_latency_watcher, NULL) != 0) {
			close(fd);
			ane_lat.disabled = 1;
			return;
		}
		pthread_detach(tid);
		ane_lat.watcher_running = 1;
	}
	ane_lat.fd = fd;
	__atomic_store_n(&ane_lat.held, 1, __ATOMIC_RELEASE);
	pthread_cond_signal(&ane_lat.wake);
}

static inline void ane_latency_touch(void)
{
	__atomic_store_n(&ane_lat.last_ns, ane_latency_now(), __ATOMIC_RELAXED);
	if (__atomic_load_n(&ane_lat.held, __ATOMIC_ACQUIRE)) {
		return;
	}
	pthread_mutex_lock(&ane_lat.lock);
	if (!ane_lat.disabled && ane_lat.fd < 0) {
		ane_latency_open();
	}
	pthread_mutex_unlock(&ane_lat.lock);
}

#endif
