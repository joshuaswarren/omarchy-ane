// GapWinA E2 memcpy hog: 8 threads, 2x128 MiB buffers each, duty-cycled by a
// controller targeting <= 55 GB/s aggregate (cap 60 = 15% of the 400 GB/s spec).
// Logs "<t_s> <GB/s> <sleep_us>" to the file given as argv[1] every second.
// gcc -O2 -o hog hog.c -lpthread
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NTH 8
#define BUFSZ (128u << 20)
#define CHUNK (8u << 20)
#define TARGET 55e9
#define CAP 60e9

static atomic_ullong bytes;
static atomic_int sleep_us;
static _Atomic double rate_gbs;
static char *src[NTH], *dst[NTH];

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void *worker(void *arg)
{
	long id = (long)arg;
	while (1) {
		for (size_t off = 0; off < BUFSZ; off += CHUNK) {
			memcpy(dst[id] + off, src[id] + off, CHUNK);
			atomic_fetch_add(&bytes, CHUNK);
		}
		int us = atomic_load(&sleep_us);
		if (us) {
			struct timespec ts = { us / 1000000, (us % 1000000) * 1000 };
			nanosleep(&ts, NULL);
		}
	}
	return NULL;
}

int main(int argc, char **argv)
{
	FILE *log = stdout;
	if (argc > 1)
		log = fopen(argv[1], "w");
	for (int i = 0; i < NTH; i++) {
		src[i] = malloc(BUFSZ);
		dst[i] = malloc(BUFSZ);
		memset(src[i], i, BUFSZ);
	}
	pthread_t th[NTH];
	for (long i = 0; i < NTH; i++)
		pthread_create(&th[i], NULL, worker, (void *)i);
	double t0 = now_s(), last = t0;
	unsigned long long b0 = 0, bprev = 0;
	int us = 0;
	atomic_store(&sleep_us, 0);
	while (1) {
		struct timespec ts = { 0, 250 * 1000 * 1000 };
		nanosleep(&ts, NULL);
		double t = now_s();
		unsigned long long b = atomic_load(&bytes);
		double inst = (b - bprev) / (t - last); /* bytes/s instant */
		double agg = (b - b0) / (t - t0);       /* effective since start */
		bprev = b; last = t; rate_gbs = inst / 1e9;
		if (t - t0 > 1.0) {
			/* duty-cycle control on the effective rate */
			if (agg > TARGET && us < 40000)
				us += 400;
			else if (agg < TARGET * 0.8 && us > 0)
				us -= 200;
			atomic_store(&sleep_us, us);
			if (log)
				fprintf(log, "%.1f %.1f %d\n", t - t0, agg / 1e9, us), fflush(log);
		}
		if (agg > CAP) { /* safety: never exceed the 15% cap */
			us += 2000;
			atomic_store(&sleep_us, us);
		}
		(void)b0;
	}
	return 0;
}
