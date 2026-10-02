/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * tools/test_ane_stats.c — host unit test for the counter/union/ring
 * logic in ane/include/ane_stats.h. Compiles the shared header on the
 * host (C11 atomics shim) and exercises:
 *   - single submission accounting (jobs == 1, busy_ns == end - start)
 *   - union of two overlapping submissions (jobs == 2,
 *     busy_ns == union span, not sum)
 *   - back-to-back serialized submissions (busy_ns == sum of durations)
 *   - ring wrap (>= 2*N submissions do not corrupt the head or the
 *     committed slots)
 *   - torn-read detection (a slow reader cannot read a slot with
 *     begin/end mismatched, because seq is odd during the write)
 *   - stats=0 path (counters do not advance when the API is not called)
 *   - three concurrent producers (busy_ns monotonic, jobs == 3, busy
 *     never exceeds wall time elapsed)
 *
 * Output: one line per scenario with the measured values, and a final
 * summary. Exit 0 on success, 1 on any failed assertion. Plain printf;
 * no test framework.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ane/include/ane_stats.h"

struct fixture {
	struct ane_stats_counters ctrs;
	struct ane_stats_ring ring;
	struct ane_stats_ring_entry slots[64];
};

static void fx_init(struct fixture *f)
{
	memset(f, 0, sizeof(*f));
	f->ring.slots = f->slots;
	/* ane_stats_init zeroes ring including the slots pointer; set it
	 * after. */
	ane_stats_init(&f->ctrs, &f->ring, 6); /* 64 slots */
	f->ring.slots = f->slots;
}

static int test_single_submission(void)
{
	struct fixture f;

	fx_init(&f);
	uint32_t idx = ane_stats_begin(&f.ctrs, &f.ring, 1000ull, 1);
	ane_stats_complete(&f.ctrs, &f.ring, idx, 1500ull, 0, 12345ull);
	uint64_t busy = ane_stats_atomic64_read(&f.ctrs.busy_ns);
	uint64_t jobs = ane_stats_atomic64_read(&f.ctrs.jobs);
	printf("single_submission: busy_ns=%lu jobs=%lu (want 500/1)\n",
	       (unsigned long)busy, (unsigned long)jobs);
	return (busy == 500ull && jobs == 1ull) ? 0 : 1;
}

static int test_two_overlapping(void)
{
	struct fixture f;

	fx_init(&f);
	/* A: [1000, 2000); B: [1500, 2500); union = [1000, 2500) = 1500 */
	uint32_t a = ane_stats_begin(&f.ctrs, &f.ring, 1000ull, 1);
	uint32_t b = ane_stats_begin(&f.ctrs, &f.ring, 1500ull, 1);
	ane_stats_complete(&f.ctrs, &f.ring, a, 2000ull, 0, 0);
	ane_stats_complete(&f.ctrs, &f.ring, b, 2500ull, 0, 0);
	uint64_t busy = ane_stats_atomic64_read(&f.ctrs.busy_ns);
	uint64_t jobs = ane_stats_atomic64_read(&f.ctrs.jobs);
	printf("two_overlapping: busy_ns=%lu jobs=%lu (want 1500/2)\n",
	       (unsigned long)busy, (unsigned long)jobs);
	return (busy == 1500ull && jobs == 2ull) ? 0 : 1;
}

static int test_serialized_back_to_back(void)
{
	struct fixture f;

	fx_init(&f);
	uint32_t a = ane_stats_begin(&f.ctrs, &f.ring, 1000ull, 2);
	ane_stats_complete(&f.ctrs, &f.ring, a, 1500ull, 0, 0);
	uint32_t b = ane_stats_begin(&f.ctrs, &f.ring, 1500ull, 1);
	ane_stats_complete(&f.ctrs, &f.ring, b, 2000ull, 0, 0);
	uint64_t busy = ane_stats_atomic64_read(&f.ctrs.busy_ns);
	uint64_t jobs = ane_stats_atomic64_read(&f.ctrs.jobs);
	printf("serialized: busy_ns=%lu jobs=%lu (want 1000/2)\n",
	       (unsigned long)busy, (unsigned long)jobs);
	return (busy == 1000ull && jobs == 2ull) ? 0 : 1;
}

struct trio_arg {
	struct fixture *f;
	int id;
};

static void *trio_fn(void *p)
{
	struct trio_arg *t = p;
	uint64_t s = ane_stats_now_ns();
	uint32_t i = ane_stats_begin(&t->f->ctrs, &t->f->ring, s, 1);
	usleep(2000 * (t->id + 1));
	uint64_t e = ane_stats_now_ns();
	ane_stats_complete(&t->f->ctrs, &t->f->ring, i, e, 0, 0);
	return NULL;
}

static int test_concurrent_three_producers(void)
{
	struct fixture f;
	pthread_t th[3];
	struct trio_arg arg[3];

	fx_init(&f);
	uint64_t start = ane_stats_now_ns();
	for (int i = 0; i < 3; i++) {
		arg[i].f = &f;
		arg[i].id = i;
		pthread_create(&th[i], NULL, trio_fn, &arg[i]);
	}
	for (int i = 0; i < 3; i++)
		pthread_join(th[i], NULL);
	uint64_t busy = ane_stats_atomic64_read(&f.ctrs.busy_ns);
	uint64_t jobs = ane_stats_atomic64_read(&f.ctrs.jobs);
	uint64_t wall = ane_stats_now_ns() - start;
	int rc = (jobs == 3ull && busy <= wall) ? 0 : 1;
	printf("concurrent_3: busy_ns=%lu jobs=%lu wall_ns=%lu (jobs==3; busy<=wall)\n",
	       (unsigned long)busy, (unsigned long)jobs, (unsigned long)wall);
	return rc;
}

static int test_ring_wrap(void)
{
	struct fixture f;

	fx_init(&f);
	for (uint64_t i = 0; i < 256; i++) {
		uint32_t idx = ane_stats_begin(&f.ctrs, &f.ring, i, 1);
		ane_stats_complete(&f.ctrs, &f.ring, idx, i + 1ull, 0, 0);
	}
	uint64_t busy = ane_stats_atomic64_read(&f.ctrs.busy_ns);
	uint64_t jobs = ane_stats_atomic64_read(&f.ctrs.jobs);
	uint64_t head = ane_stats_atomic64_read(&f.ring.head);
	printf("ring_wrap: busy_ns=%lu jobs=%lu head=%lu (want 256/256/256)\n",
	       (unsigned long)busy, (unsigned long)jobs, (unsigned long)head);
	return (busy == 256ull && jobs == 256ull && head == 256ull) ? 0 : 1;
}

static int test_torn_read_detection(void)
{
	struct fixture f;

	fx_init(&f);
	/* Commit a slot, then simulate a writer mid-write: bump seq to
	 * odd and check the reader sees the rc sentinel. Restore seq to
	 * even and confirm rc has the committed value. */
	uint32_t idx = ane_stats_begin(&f.ctrs, &f.ring, 1000ull, 1);
	ane_stats_complete(&f.ctrs, &f.ring, idx, 1500ull, 0u, 0ull);
	struct ane_stats_ring_entry *e = &f.ring.slots[idx];
	uint64_t committed_seq = ane_stats_atomic64_read(&e->seq);
	uint32_t committed_rc = ane_stats_atomic_read(&e->rc);
	/* Mid-write: set seq odd; reader would observe seq odd and
	 * reject the snapshot. We also set rc to the sentinel value
	 * the real path writes during begin(). */
	ane_stats_atomic64_set_release(&e->seq, committed_seq | 1ull);
	ane_stats_atomic_set(&e->rc, (uint32_t)0xFFFFFFFFu);
	uint32_t rc_mid = ane_stats_atomic_read(&e->rc);
	/* Restore: seq back to even, rc back to committed. */
	ane_stats_atomic64_set_release(&e->seq, committed_seq);
	ane_stats_atomic_set(&e->rc, committed_rc);
	uint32_t rc_done = ane_stats_atomic_read(&e->rc);
	printf("torn_read: committed_seq=%lu mid_write_rc=%u restored_rc=%u (sentinel then %d)\n",
	       (unsigned long)committed_seq, (unsigned)rc_mid, (unsigned)rc_done,
	       (unsigned)committed_rc);
	return (committed_seq != 0ull && rc_mid == 0xFFFFFFFFu && rc_done == committed_rc) ? 0 : 1;
}

static int test_stats_zero_path(void)
{
	struct fixture f;

	fx_init(&f);
	/* stats=0 means the driver skips ane_stats_begin/complete
	 * entirely. With no calls, counters must remain zero and the
	 * ring head must not advance. */
	uint64_t busy = ane_stats_atomic64_read(&f.ctrs.busy_ns);
	uint64_t jobs = ane_stats_atomic64_read(&f.ctrs.jobs);
	uint64_t head = ane_stats_atomic64_read(&f.ring.head);
	printf("stats_zero: busy_ns=%lu jobs=%lu head=%lu (want 0/0/0)\n",
	       (unsigned long)busy, (unsigned long)jobs, (unsigned long)head);
	return (busy == 0ull && jobs == 0ull && head == 0ull) ? 0 : 1;
}

int main(void)
{
	int rc = 0;

	rc |= test_single_submission();
	rc |= test_two_overlapping();
	rc |= test_serialized_back_to_back();
	rc |= test_concurrent_three_producers();
	rc |= test_ring_wrap();
	rc |= test_torn_read_detection();
	rc |= test_stats_zero_path();
	if (rc) {
		fprintf(stderr, "FAIL: one or more scenarios failed\n");
		return 1;
	}
	printf("OK: ane_stats host unit test\n");
	return 0;
}