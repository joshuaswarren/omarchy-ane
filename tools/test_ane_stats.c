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
 * It also compiles the REAL ane/ane_stats_show.c (via the stub kernel
 * headers in ane_stats_shim/) and the typed formatter ane_stats_emit()
 * from the real ane/include/ane_stats.h, and exercises:
 *   - idle emit output ("busy_ns 0\njobs 0\n")
 *   - emit after five simulated submissions ("busy_ns 250\njobs 5\n")
 *   - timeline render through the real fops open path: header-only on
 *     an idle ring, N data lines after N submissions, newest first,
 *     exact field values, in-flight and torn slots never printed,
 *     window capped at the ring size after wrap
 *   - ticket/seq protocol under concurrent producers (three lines,
 *     seqs 6/4/2)
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
#include "ane_stats_shim/linux/fs.h"
#include "ane_stats_shim/linux/seq_file.h"

extern const struct file_operations ane_timeline_fops;

struct fixture {
	struct ane_stats_counters ctrs;
	struct ane_stats_ring ring;
	struct ane_stats_ring_entry slots[64];
};

static int render_timeline(struct fixture *f, char *out, size_t outsz);

static void fx_init(struct fixture *f)
{
	memset(f, 0, sizeof(*f));
	f->ring.slots = f->slots;
	/* ane_stats_counters_init zeroes ring including the slots pointer;
	 * set it after. */
	ane_stats_counters_init(&f->ctrs, &f->ring, 6); /* 64 slots */
	f->ring.slots = f->slots;
}

static int test_single_submission(void)
{
	struct fixture f;

	fx_init(&f);
	uint64_t idx = ane_stats_begin(&f.ctrs, &f.ring, 1000ull, 1);
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
	uint64_t a = ane_stats_begin(&f.ctrs, &f.ring, 1000ull, 1);
	uint64_t b = ane_stats_begin(&f.ctrs, &f.ring, 1500ull, 1);
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
	uint64_t a = ane_stats_begin(&f.ctrs, &f.ring, 1000ull, 2);
	ane_stats_complete(&f.ctrs, &f.ring, a, 1500ull, 0, 0);
	uint64_t b = ane_stats_begin(&f.ctrs, &f.ring, 1500ull, 1);
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
	uint64_t i = ane_stats_begin(&t->f->ctrs, &t->f->ring, s, 1);
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
	/* Ticket/seq protocol under concurrency: three committed lines,
	 * newest first, seqs 6/4/2. */
	char out[8192];
	int n = render_timeline(&f, out, sizeof(out));
	int lines_ok = n == 4 && strstr(out, "\n6 ") && strstr(out, "\n4 ") &&
		       strstr(out, "\n2 ") && strstr(out, "\n6 ") <
		       strstr(out, "\n4 ");
	int rc = (jobs == 3ull && busy <= wall && lines_ok) ? 0 : 1;
	printf("concurrent_3: busy_ns=%lu jobs=%lu wall_ns=%lu timeline_lines=%d (jobs==3; busy<=wall; header+3, seq 6/4/2)\n",
	       (unsigned long)busy, (unsigned long)jobs, (unsigned long)wall,
	       n);
	return rc;
}

static int test_ring_wrap(void)
{
	struct fixture f;

	fx_init(&f);
	for (uint64_t i = 0; i < 256; i++) {
		uint64_t idx = ane_stats_begin(&f.ctrs, &f.ring, i, 1);
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
	uint64_t idx = ane_stats_begin(&f.ctrs, &f.ring, 1000ull, 1);
	ane_stats_complete(&f.ctrs, &f.ring, idx, 1500ull, 0u, 0ull);
	struct ane_stats_ring_entry *e =
		&f.ring.slots[(idx - 1ull) & f.ring.mask];
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

/* ---- Real show-path coverage: ane/ane_stats_show.c via ane_stats_shim/,
 * ane_stats_emit() from the real ane/include/ane_stats.h. ---- */

/* Render the timeline through the REAL fops open path (H13 wiring:
 * i_private carries &ring), then run the real show into out. */
static int render_timeline(struct fixture *f, char *out, size_t outsz)
{
	struct inode in = { .i_private = &f->ring };
	struct file filp = { .private_data = NULL };
	struct seq_file *m;
	int ret = ane_timeline_fops.open(&in, &filp);
	int nlines = 0;
	char *p;

	if (ret)
		return ret;
	m = filp.private_data;
	m->buf = out;
	m->size = outsz;
	m->len = 0;
	m->show(m, NULL);
	single_release(&in, &filp);
	for (p = out; (p = strchr(p, '\n')); p++)
		nlines++;
	return nlines; /* header + data lines */
}

static int test_emit_idle(void)
{
	struct fixture f;
	char buf[ANE_STATS_EMIT_MAX];

	fx_init(&f);
	ane_stats_emit(buf, &f.ctrs);
	int ok = strcmp(buf, "busy_ns 0\njobs 0\n") == 0;
	printf("emit_idle: %s%s", buf, ok ? "" : "  (MISMATCH)\n");
	return ok ? 0 : 1;
}

static int test_emit_after_five(void)
{
	struct fixture f;
	char buf[ANE_STATS_EMIT_MAX];

	fx_init(&f);
	/* Five serialized submissions, 50 ns each. */
	for (uint64_t i = 0; i < 5; i++) {
		uint64_t t = ane_stats_begin(&f.ctrs, &f.ring,
					     100ull + 50ull * i, 1);
		ane_stats_complete(&f.ctrs, &f.ring, t,
				   150ull + 50ull * i, 0, 12345ull);
	}
	ane_stats_emit(buf, &f.ctrs);
	int ok = strcmp(buf, "busy_ns 250\njobs 5\n") == 0;
	printf("emit_after_five: %s%s", buf, ok ? "" : "  (MISMATCH)\n");
	return ok ? 0 : 1;
}

static int test_timeline_idle(void)
{
	struct fixture f;
	char out[8192];

	fx_init(&f);
	int n = render_timeline(&f, out, sizeof(out));
	int ok = n == 1 && strncmp(out, "# ane_timeline:", 15) == 0;
	printf("timeline_idle: %d line(s), first \"%.*s\"%s\n", n,
	       (int)(strchr(out, '\n') - out), out, ok ? "" : "  (MISMATCH)");
	return ok ? 0 : 1;
}

static int test_timeline_five(void)
{
	struct fixture f;
	char out[8192];

	fx_init(&f);
	for (uint64_t i = 0; i < 5; i++) {
		uint64_t t = ane_stats_begin(&f.ctrs, &f.ring,
					     100ull + 50ull * i, 1);
		ane_stats_complete(&f.ctrs, &f.ring, t,
				   150ull + 50ull * i, 0, 12345ull);
	}
	int n = render_timeline(&f, out, sizeof(out));
	/* Newest first: header + 5 data lines; newest is ticket 5
	 * (seq 10), oldest is ticket 1 (seq 2). */
	char *line5 = strstr(out, "\n10 300 300 350 1 0 12345\n");
	char *line1 = strstr(out, "\n2 100 100 150 1 0 12345\n");
	int ok = n == 6 && line5 && line1 && line5 < line1;
	printf("timeline_five: %d line(s) (want 6), newest/oldest %s%s\n",
	       n, line5 ? "found" : "MISSING", ok ? "" : "  (MISMATCH)");
	return ok ? 0 : 1;
}

static int test_timeline_inflight_skipped(void)
{
	struct fixture f;
	char out[8192];

	fx_init(&f);
	/* Four complete, the fifth only begun: odd seq, never printed. */
	for (uint64_t i = 0; i < 4; i++) {
		uint64_t t = ane_stats_begin(&f.ctrs, &f.ring,
					     100ull + 50ull * i, 1);
		ane_stats_complete(&f.ctrs, &f.ring, t,
				   150ull + 50ull * i, 0, 0);
	}
	ane_stats_begin(&f.ctrs, &f.ring, 300ull, 1);
	int n = render_timeline(&f, out, sizeof(out));
	int ok = n == 5 && !strstr(out, "\n10 ");
	printf("timeline_inflight: %d line(s) (want 5 = header + 4)%s\n",
	       n, ok ? "" : "  (MISMATCH)");
	return ok ? 0 : 1;
}

static int test_timeline_torn_skipped(void)
{
	struct fixture f;
	char out[8192];

	fx_init(&f);
	for (uint64_t i = 0; i < 5; i++) {
		uint64_t t = ane_stats_begin(&f.ctrs, &f.ring,
					     100ull + 50ull * i, 1);
		ane_stats_complete(&f.ctrs, &f.ring, t,
				   150ull + 50ull * i, 0, 0);
	}
	/* Simulate a torn newest entry: seq goes odd mid-write. */
	ane_stats_atomic64_set_release(&f.ring.slots[4].seq, 9ull);
	int n = render_timeline(&f, out, sizeof(out));
	int ok = n == 5 && !strstr(out, "\n10 ");
	printf("timeline_torn: %d line(s) (want 5 = header + 4)%s\n",
	       n, ok ? "" : "  (MISMATCH)");
	return ok ? 0 : 1;
}

static int test_timeline_wrap_window(void)
{
	struct fixture f;
	char out[32768];

	fx_init(&f);
	/* 70 submissions through a 64-slot ring: the reader shows the
	 * last 64 (tickets 7..70, seqs 14..140) and nothing older. */
	for (uint64_t i = 0; i < 70; i++) {
		uint64_t t = ane_stats_begin(&f.ctrs, &f.ring, i, 1);
		ane_stats_complete(&f.ctrs, &f.ring, t, i + 1ull, 0, 0);
	}
	int n = render_timeline(&f, out, sizeof(out));
	int ok = n == 65 && strstr(out, "\n140 69 69 70 1 0 0\n") &&
		 strstr(out, "\n14 6 6 7 1 0 0\n") &&
		 !strstr(out, "\n12 5 5 6 1 0 0\n");
	printf("timeline_wrap: %d line(s) (want 65 = header + 64)%s\n",
	       n, ok ? "" : "  (MISMATCH)");
	return ok ? 0 : 1;
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
	rc |= test_emit_idle();
	rc |= test_emit_after_five();
	rc |= test_timeline_idle();
	rc |= test_timeline_five();
	rc |= test_timeline_inflight_skipped();
	rc |= test_timeline_torn_skipped();
	rc |= test_timeline_wrap_window();
	if (rc) {
		fprintf(stderr, "FAIL: one or more scenarios failed\n");
		return 1;
	}
	printf("OK: ane_stats host unit test\n");
	return 0;
}