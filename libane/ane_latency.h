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
 * stops dispatching gives the CPUs back their deep idle state. The process exit closes the file too. A fork() without exec closes
 * the child's inherited descriptor and resets the state, so only the dispatching process holds the request: the watcher thread does
 * not survive the fork, and the child builds its own on its first dispatch.
 *
 * ANE_CPU_LATENCY_HOLD_MS: idle time in milliseconds before the hold is dropped (default 50). 0 or negative turns the hold off.
 * Garbage parses as the default, not as off; values above a day clamp to a day.
 * ANE_CPU_LATENCY_DEV: path of the latency file (default /dev/cpu_dma_latency); the host test points it at a plain file.
 * A file that cannot be opened turns the hold off for the rest of the process. The stderr message prints only when
 * ANE_CPU_LATENCY_HOLD_MS was set explicitly: a process that never asked for the hold stays silent, because /dev/cpu_dma_latency
 * is usually root-only and default-on would print on every non-root T6021 dispatch.
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
#define ANE_LATENCY_MAX_MS 86400000	/* a day: bounds hold * 1000000 and a hold nobody meant */

struct ane_latency {
	pthread_mutex_t lock;
	pthread_cond_t wake;
	int fd;			/* -1 when no hold is active */
	int held;		/* mirrors fd >= 0 for the lock-free check */
	int disabled;
	int watcher_running;
	int cond_ready;		/* wake re-inited with a monotonic clock */
	int atfork_done;	/* child handler registered */
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

/* The idle wait uses the monotonic clock: a wall-clock step back must not extend the hold. */
static inline void ane_latency_deadline(struct timespec *ts, uint64_t ns)
{
	clock_gettime(CLOCK_MONOTONIC, ts);
	ns += ts->tv_nsec;
	ts->tv_sec += ns / 1000000000ull;
	ts->tv_nsec = ns % 1000000000ull;
}

/* Runs in a fork() child with only the calling thread: drop the inherited hold and the dead watcher's state. */
static inline void ane_latency_child(void)
{
	if (ane_lat.fd >= 0) {
		close(ane_lat.fd);
	}
	ane_lat.fd = -1;
	__atomic_store_n(&ane_lat.held, 0, __ATOMIC_RELEASE);
	ane_lat.watcher_running = 0;
	ane_lat.cond_ready = 0;
	pthread_mutex_init(&ane_lat.lock, NULL);
	pthread_cond_init(&ane_lat.wake, NULL);
}

/* ANE_CPU_LATENCY_HOLD_MS in milliseconds; unset or garbage is the default, 0 or negative turns the hold off. */
static inline long ane_latency_hold_ms(void)
{
	const char *ms = getenv("ANE_CPU_LATENCY_HOLD_MS");
	char *end;
	long hold;

	if (!ms) {
		return ANE_LATENCY_DEFAULT_MS;
	}
	errno = 0;
	hold = strtol(ms, &end, 10);
	if (end == ms || *end || errno == ERANGE) {
		return ANE_LATENCY_DEFAULT_MS;
	}
	if (hold > ANE_LATENCY_MAX_MS) {
		return ANE_LATENCY_MAX_MS;
	}
	return hold;
}

/* Waits for the idle time to pass after the latest dispatch, closes the file, then sleeps until the next dispatch opens it. */
static inline void *ane_latency_watcher(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&ane_lat.lock);
	for (;;) {
		int64_t idle;
		struct timespec ts;

		if (ane_lat.fd < 0) {
			pthread_cond_wait(&ane_lat.wake, &ane_lat.lock);
			continue;
		}
		/* Signed: a dispatch stamped after our clock read must look young, not like an eternity ago. */
		idle = (int64_t)(ane_latency_now() - __atomic_load_n(&ane_lat.last_ns, __ATOMIC_RELAXED));
		if (idle >= 0 && (uint64_t)idle >= ane_lat.idle_ns) {
			close(ane_lat.fd);
			ane_lat.fd = -1;
			__atomic_store_n(&ane_lat.held, 0, __ATOMIC_RELEASE);
			continue;
		}
		ane_latency_deadline(&ts, ane_lat.idle_ns - (uint64_t)(idle > 0 ? idle : 0));
		pthread_cond_timedwait(&ane_lat.wake, &ane_lat.lock, &ts);
	}
	return NULL;
}

static inline void ane_latency_open(void)
{
	const char *dev = getenv("ANE_CPU_LATENCY_DEV");
	int32_t zero = 0;
	long hold = ane_latency_hold_ms();
	pthread_t tid;
	int fd;

	if (hold <= 0) {
		ane_lat.disabled = 1;
		return;
	}
	ane_lat.idle_ns = (uint64_t)hold * 1000000ull;
	fd = open(dev ? dev : "/dev/cpu_dma_latency", O_WRONLY | O_CLOEXEC);
	if (fd < 0 || write(fd, &zero, sizeof(zero)) != sizeof(zero)) {
		if (getenv("ANE_CPU_LATENCY_HOLD_MS")) {
			fprintf(stderr, "LIBANE: cpu latency hold off: %s\n", strerror(errno));
		}
		if (fd >= 0) {
			close(fd);
		}
		ane_lat.disabled = 1;
		return;
	}
	if (!ane_lat.atfork_done) {
		pthread_atfork(NULL, NULL, ane_latency_child);
		ane_lat.atfork_done = 1;
	}
	if (!ane_lat.cond_ready) {
		pthread_condattr_t ca;

		pthread_condattr_init(&ca);
		pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
		pthread_cond_init(&ane_lat.wake, &ca);
		pthread_condattr_destroy(&ca);
		ane_lat.cond_ready = 1;
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
