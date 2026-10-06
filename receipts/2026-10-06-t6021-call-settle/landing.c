// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

/* Per-call output landing check for the H14 add program (nchw [1,512,1,1],
 * fp16, 64-byte plane stride: lane i is valid when i % 32 == 0).
 *
 * Eight seeded input pairs rotate call by call, so every call expects a
 * result that differs from the previous call's. The output is read right
 * after the CALL ioctl returns and compared byte for byte (valid lanes:
 * fp16 sum with ties away from zero; padding: zero). A write that lands
 * after the ioctl returns shows as a mismatch.
 *
 * usage: landing ANEC CALLS SEED
 * build: gcc -O2 -I$R/libane -I$R/tools -I$R/ane/src/uapi/drm \
 *            -o landing landing.c $R/libane/libane.a -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ane.h"
#include "ane_f16_add.h"

#define PAIRS 8

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
	uint16_t *a[PAIRS], *b[PAIRS], *want[PAIRS], *y;
	uint64_t size, n, i, calls, bad = 0, stale = 0, zero = 0;
	struct ane_nn *nn;
	double *lat;

	if (argc != 4) {
		fprintf(stderr, "usage: %s ANEC CALLS SEED\n", argv[0]);
		return 2;
	}
	calls = strtoull(argv[2], NULL, 0);
	srand48(strtol(argv[3], NULL, 0));
	nn = ane_init(argv[1]);
	if (!nn)
		return 1;
	size = ane_src_size(nn, 0);
	if (!size || ane_src_size(nn, 1) != size || ane_dst_size(nn, 0) != size) {
		fprintf(stderr, "io sizes disagree\n");
		return 1;
	}
	n = size / 2;
	for (int p = 0; p < PAIRS; p++) {
		a[p] = calloc(n, 2);
		b[p] = calloc(n, 2);
		want[p] = calloc(n, 2);
		for (i = 0; i < n; i += 32) {
			_Float16 fa = (_Float16)(drand48() * 8.0 - 4.0);
			_Float16 fb = (_Float16)(drand48() * 8.0 - 4.0);

			memcpy(&a[p][i], &fa, 2);
			memcpy(&b[p][i], &fb, 2);
			want[p][i] = ane_f16_round_half_away(ane_f16_to_f64(a[p][i]) +
							     ane_f16_to_f64(b[p][i]));
		}
	}
	y = malloc(size);
	lat = calloc(calls, sizeof(*lat));
	for (uint64_t c = 0; c < calls; c++) {
		int p = c % PAIRS;
		struct timespec t0, t1;

		ane_send(nn, a[p], 0);
		ane_send(nn, b[p], 1);
		clock_gettime(CLOCK_MONOTONIC, &t0);
		if (ane_exec(nn) < 0) {
			fprintf(stderr, "ane_exec failed at call %llu\n",
				(unsigned long long)c);
			return 1;
		}
		clock_gettime(CLOCK_MONOTONIC, &t1);
		ane_read(nn, y, 0);
		lat[c] = (t1.tv_sec - t0.tv_sec) * 1e3 +
			 (t1.tv_nsec - t0.tv_nsec) / 1e6;
		if (memcmp(y, want[p], size)) {
			uint64_t nz = 0;

			bad++;
			stale += c && !memcmp(y, want[(c - 1) % PAIRS], size);
			for (i = 0; i < n; i++)
				nz += y[i] != 0;
			zero += !nz;
			if (bad <= 5)
				fprintf(stderr, "call %llu: mismatch (stale %d, zero %d)\n",
					(unsigned long long)c,
					c && !memcmp(y, want[(c - 1) % PAIRS], size),
					!nz);
		}
	}
	qsort(lat, calls, sizeof(*lat), cmp_double);
	printf("calls %llu mismatched %llu stale %llu all-zero %llu\n",
	       (unsigned long long)calls, (unsigned long long)bad,
	       (unsigned long long)stale, (unsigned long long)zero);
	printf("exec ms: min %.3f median %.3f p90 %.3f p99 %.3f max %.3f\n",
	       lat[0], lat[calls / 2], lat[calls * 9 / 10],
	       lat[calls * 99 / 100], lat[calls - 1]);
	ane_free(nn);
	return bad ? 1 : 0;
}
