/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Joshua Warren */

/* ane-cycle: H13 (ABI-1) driver lifecycle loop. Each cycle is a full
 * ane_init(program load) -> one exec -> ane_free, in ONE process, so the
 * per-cycle cost of the load/release path is observable separately from the
 * per-call cost (tools/ane-run --repeat holds one ane_init for all repeats
 * and frees once at exit, so it cannot express this loop).
 *
 *   ane-cycle --anec F --cycles N [--in IDX=FILE]... [--expect FILE]
 *
 * Inputs are raw channel bytes in libane index order (the convention of
 * tools/ane-session-h13.c and the soak/gate scripts), sized from libane at
 * first init. With --expect, output bytes are compared bit-exact every cycle
 * and any mismatch fails the run at that cycle. Per cycle one line:
 *
 *   CYCLE <i> init_us <us> exec_us <us> match <1|0>
 *
 * Exit 0 only if every cycle initialised, executed and (with --expect)
 * matched. Buffers are allocated once after the first init and reused, so
 * the only per-cycle allocations belong to libane itself - the subject under
 * test. No device is opened when --cycles 0.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ane.h"

#define ANE_CYCLE_MAX_CH 8

static uint64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000ull +
	       (uint64_t)ts.tv_nsec / 1000ull;
}

static int read_into(const char *path, uint8_t *buf, uint64_t want)
{
	FILE *f;
	size_t got;

	f = fopen(path, "rb");
	if (!f)
		return -1;
	got = fread(buf, 1, (size_t)want, f);
	fclose(f);
	return got == (size_t)want ? 0 : -1;
}

static int fail(const char *what, uint32_t cycle)
{
	fprintf(stderr, "FAIL %s at cycle %u\n", what, cycle);
	return 1;
}

int main(int argc, char **argv)
{
	const char *anec = NULL;
	const char *expect_path = NULL;
	const char *in_path[ANE_CYCLE_MAX_CH] = { NULL };
	uint32_t cycles = 0;
	uint32_t n_in, n_out, k, c;
	uint64_t in_total = 0, out_total = 0, off;
	uint64_t in_size[ANE_CYCLE_MAX_CH], out_size[ANE_CYCLE_MAX_CH];
	uint64_t t_init0, t_init1, t_exec0, t_exec1;
	uint8_t *in_buf = NULL, *out_buf = NULL, *expect_buf = NULL;
	struct ane_nn *nn = NULL;
	int match;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--anec") && i + 1 < argc) {
			anec = argv[++i];
		} else if (!strcmp(argv[i], "--cycles") && i + 1 < argc) {
			cycles = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--expect") && i + 1 < argc) {
			expect_path = argv[++i];
		} else if (!strncmp(argv[i], "--in ", 5) && i + 1 < argc) {
			unsigned idx = (unsigned)strtoul(argv[i] + 5, NULL, 0);

			if (idx >= ANE_CYCLE_MAX_CH)
				return fail("channel index out of range", 0);
			in_path[idx] = argv[++i];
		} else {
			fprintf(stderr,
				"usage: ane-cycle --anec F --cycles N "
				"[--in IDX=FILE]... [--expect FILE]\n");
			return 2;
		}
	}
	if (!anec || !cycles)
		return fail("missing --anec or --cycles", 0);

	for (c = 1; c <= cycles; c++) {
		t_init0 = now_us();
		nn = ane_init(anec);
		t_init1 = now_us();
		if (!nn)
			return fail("ane_init", c);

		n_in = ane_src_count(nn);
		n_out = ane_dst_count(nn);
		if (!n_in || n_in > ANE_CYCLE_MAX_CH)
			return fail("input channel count", c);
		if (!n_out || n_out > ANE_CYCLE_MAX_CH)
			return fail("output channel count", c);

		if (!in_buf) {
			for (k = 0; k < n_in; k++) {
				if (!in_path[k])
					return fail("missing --in file", c);
				in_size[k] = __ane_src_size(nn, k);
				in_total += in_size[k];
			}
			for (k = 0; k < n_out; k++) {
				out_size[k] = __ane_dst_size(nn, k);
				out_total += out_size[k];
			}
			in_buf = malloc((size_t)in_total);
			out_buf = malloc((size_t)out_total);
			if (!in_buf || !out_buf)
				return fail("alloc", c);
			off = 0;
			for (k = 0; k < n_in; k++) {
				if (read_into(in_path[k], in_buf + off,
					      in_size[k]))
					return fail("read input", c);
				off += in_size[k];
			}
			if (expect_path) {
				expect_buf = malloc((size_t)out_total);
				if (!expect_buf)
					return fail("alloc expect", c);
				if (read_into(expect_path, expect_buf,
					      out_total))
					return fail("read expect", c);
			}
		} else {
			/* io sizes must be identical every cycle */
			uint64_t it = 0, ot = 0;

			for (k = 0; k < n_in; k++)
				it += __ane_src_size(nn, k);
			for (k = 0; k < n_out; k++)
				ot += __ane_dst_size(nn, k);
			if (it != in_total || ot != out_total)
				return fail("io size changed across cycles",
					    c);
		}

		off = 0;
		for (k = 0; k < n_in; k++) {
			__ane_send(nn, in_buf + off, k);
			off += in_size[k];
		}
		t_exec0 = now_us();
		if (ane_exec(nn) < 0)
			return fail("ane_exec", c);
		t_exec1 = now_us();
		off = 0;
		for (k = 0; k < n_out; k++) {
			__ane_read(nn, out_buf + off, k);
			off += out_size[k];
		}
		match = expect_buf ? (memcmp(out_buf, expect_buf,
					     (size_t)out_total) == 0) : 1;
		ane_free(nn);
		nn = NULL;
		printf("CYCLE %u init_us %llu exec_us %llu match %d\n", c,
		       (unsigned long long)(t_init1 - t_init0),
		       (unsigned long long)(t_exec1 - t_exec0), match);
		fflush(stdout);
		if (expect_buf && !match)
			return fail("bit-exact mismatch", c);
	}
	free(in_buf);
	free(out_buf);
	free(expect_buf);
	return 0;
}
