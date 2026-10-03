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
 *   - continuous busy_ns: the value advances while a submission is in
 *     flight (live tail), a closed gap between periods accrues nothing,
 *     and a fold only lands when the last submission of a period
 *     completes
 *   - randomized four-producer stress: snapshot reads never decrease,
 *     never exceed wall time, and the final busy_ns equals the exact
 *     union of all submit-to-completion intervals with jobs == the
 *     number of completions
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

/* ---- Continuous busy_ns (round 3): live tail, gaps, folds. ---- */

static int test_emit_live_tail(void)
{
	struct fixture f;
	char buf[ANE_STATS_EMIT_MAX], want[ANE_STATS_EMIT_MAX];
	uint64_t t0, end_at, v, wall;
	uint64_t t;

	fx_init(&f);
	t0 = ane_stats_now_ns();
	t = ane_stats_begin(&f.ctrs, &f.ring, t0, 1);
	usleep(4000);
	v = ane_stats_snapshot(&f.ctrs);
	wall = ane_stats_now_ns() - t0;
	if (v < 4000000ull || v > wall) {
		printf("live_tail: in-flight snapshot %lu outside [%lu, %lu]\n",
		       (unsigned long)v, 4000000ul, (unsigned long)wall);
		return 1;
	}
	end_at = ane_stats_now_ns();
	ane_stats_complete(&f.ctrs, &f.ring, t, end_at, 0, 0);
	v = ane_stats_snapshot(&f.ctrs);
	/* After the close the value is the folded period, exactly. */
	snprintf(want, sizeof(want), "busy_ns %lu\njobs 1\n",
		 (unsigned long)(end_at - t0));
	ane_stats_emit(buf, &f.ctrs);
	int ok = v == end_at - t0 && strcmp(buf, want) == 0;
	printf("live_tail: in-flight >=4ms ok, closed busy_ns=%lu exact, emit \"%s%s\"",
	       (unsigned long)v, buf, ok ? "" : "  (MISMATCH)\n");
	return ok ? 0 : 1;
}

static int test_period_gap_no_accrual(void)
{
	struct fixture f;
	uint64_t t, v;

	fx_init(&f);
	t = ane_stats_begin(&f.ctrs, &f.ring, 1000ull, 1);
	ane_stats_complete(&f.ctrs, &f.ring, t, 1100ull, 0, 0);
	/* Idle between periods: the real-time now must not leak into
	 * busy_ns, and the closed 100ns period is all that shows. */
	v = ane_stats_snapshot(&f.ctrs);
	if (v != 100ull) {
		printf("period_gap: idle snapshot %lu (want 100)\n",
		       (unsigned long)v);
		return 1;
	}
	t = ane_stats_begin(&f.ctrs, &f.ring, 1500ull, 1);
	ane_stats_complete(&f.ctrs, &f.ring, t, 2000ull, 0, 0);
	v = ane_stats_snapshot(&f.ctrs);
	printf("period_gap: busy_ns=%lu (want 600 = union with gap)\n",
	       (unsigned long)v);
	return v == 600ull ? 0 : 1;
}

static int test_fold_only_at_drain(void)
{
	struct fixture f;
	uint64_t a, b, raw;

	fx_init(&f);
	a = ane_stats_begin(&f.ctrs, &f.ring, 1000ull, 1);
	b = ane_stats_begin(&f.ctrs, &f.ring, 1500ull, 1);
	ane_stats_complete(&f.ctrs, &f.ring, a, 2000ull, 0, 0);
	/* b still in flight: nothing folded yet, the period stays open. */
	raw = ane_stats_atomic64_read(&f.ctrs.busy_ns);
	ane_stats_complete(&f.ctrs, &f.ring, b, 2500ull, 0, 0);
	uint64_t busy = ane_stats_atomic64_read(&f.ctrs.busy_ns);
	printf("fold_at_drain: mid-period busy_ns=%lu (want 0), final=%lu (want 1500)\n",
	       (unsigned long)raw, (unsigned long)busy);
	return (raw == 0ull && busy == 1500ull) ? 0 : 1;
}

#define STRESS_PRODUCERS 4
#define STRESS_ITERS 250

/* 4 x 250 = 1000 jobs through a 1024-slot ring: no slot is reused, so
 * the consumed per-slot windows can be read race-free after join. */
struct stress_slot_fix {
	struct ane_stats_counters ctrs;
	struct ane_stats_ring ring;
	struct ane_stats_ring_entry slots[1024];
};

static struct stress_slot_fix stress_fx;

struct stress_producer {
	struct fixture *f;
	unsigned int seed;
};

static void *stress_fn(void *p)
{
	struct stress_producer *sp = p;

	for (int i = 0; i < STRESS_ITERS; i++) {
		uint64_t s = ane_stats_now_ns();
		uint64_t t = ane_stats_begin(&sp->f->ctrs, &sp->f->ring, s, 1);

		usleep(rand_r(&sp->seed) % 400);
		ane_stats_complete(&sp->f->ctrs, &sp->f->ring, t,
				   ane_stats_now_ns(), 0, 0);
		usleep(rand_r(&sp->seed) % 200);
	}
	return NULL;
}

struct stress_reader {
	struct fixture *f;
	uint64_t t0;
	int failed;
};

static void *stress_reader_fn(void *p)
{
	struct stress_reader *sr = p;
	uint64_t prev = 0;

	for (int i = 0; i < 20000; i++) {
		uint64_t v = ane_stats_snapshot(&sr->f->ctrs);
		uint64_t now_after = ane_stats_now_ns();

		/* Strict global monotonicity is unachievable with sampled
		 * completion times: the drain folds to the last completion
		 * SAMPLE, which can lag a concurrent reader's clock by the
		 * sample-to-drain window (observed <= 240 ns here,
		 * scheduler-bounded in general). The contract needs the dip
		 * to stay far below the 100 ms sampler tick, so bound the
		 * drawdown instead; the exact accounting property is the
		 * quiescent busy == consumed-slot-union check. */
		if (v + 10000000ull < prev) {
			sr->failed = 1;
			fprintf(stderr, "reader: drawdown v=%lu prev=%lu\n", (unsigned long)v, (unsigned long)prev);
			return NULL;
		}
		if (v > now_after - sr->t0) {
			sr->failed = 1;
			fprintf(stderr, "reader: overwall v=%lu bound=%lu\n", (unsigned long)v, (unsigned long)(now_after - sr->t0));
			return NULL;
		}
		prev = v;
	}
	return NULL;
}

static int cmp_interval_start(const void *pa, const void *pb)
{
	const struct ane_stats_ring_entry *a = pa, *b = pb;

	return (a->start_ns > b->start_ns) - (a->start_ns < b->start_ns);
}

static int test_randomized_stress(void)
{
	struct stress_producer prod[STRESS_PRODUCERS];
	struct stress_reader rdr;
	pthread_t th[STRESS_PRODUCERS], rth;
	uint64_t t0 = ane_stats_now_ns();
	int rc = 0;

	memset(&stress_fx, 0, sizeof(stress_fx));
	stress_fx.ring.slots = stress_fx.slots;
	ane_stats_counters_init(&stress_fx.ctrs, &stress_fx.ring, 10);
	stress_fx.ring.slots = stress_fx.slots;
	rdr.f = (struct fixture *)&stress_fx;
	rdr.t0 = t0;
	rdr.failed = 0;
	for (int i = 0; i < STRESS_PRODUCERS; i++) {
		prod[i].f = (struct fixture *)&stress_fx;
		prod[i].seed = 0xabeed + (unsigned int)i;
		pthread_create(&th[i], NULL, stress_fn, &prod[i]);
	}
	pthread_create(&rth, NULL, stress_reader_fn, &rdr);
	for (int i = 0; i < STRESS_PRODUCERS; i++)
		pthread_join(th[i], NULL);
	pthread_join(rth, NULL);

	uint64_t jobs = ane_stats_atomic64_read(&stress_fx.ctrs.jobs);
	uint64_t busy = ane_stats_atomic64_read(&stress_fx.ctrs.busy_ns);
	/* Reference: the consumed windows the implementation recorded
	 * per slot (start_ns = period latch, drainer end_ns = fold end),
	 * read race-free after join. */
	qsort(stress_fx.slots, STRESS_PRODUCERS * STRESS_ITERS,
	      sizeof(stress_fx.slots[0]), cmp_interval_start);
	uint64_t union_ns = 0, lo = 0, hi = 0;
	int have = 0;
	for (size_t i = 0; i < STRESS_PRODUCERS * STRESS_ITERS; i++) {
		uint64_t s = stress_fx.slots[i].start_ns;
		uint64_t e = stress_fx.slots[i].end_ns;

		/* A joiner's end sample can predate the period latch
		 * (sampled, then stalled): its counted window is empty,
		 * not negative. */
		if (e < s)
			e = s;
		if (!have || s > hi) {
			union_ns += hi - lo;
			lo = s;
			hi = e;
			have = 1;
		} else if (e > hi) {
			hi = e;
		}
	}
	if (have)
		union_ns += hi - lo;

	if (rdr.failed) {
		printf("stress: reader saw a decreasing or over-wall value\n");
		rc = 1;
	}
	if (jobs != STRESS_PRODUCERS * STRESS_ITERS) {
		printf("stress: jobs %lu (want %d)\n", (unsigned long)jobs,
		       STRESS_PRODUCERS * STRESS_ITERS);
		rc = 1;
	}
	if (busy != union_ns) {
		printf("stress: busy_ns %lu != consumed slot union %lu (delta %ld)\n",
		       (unsigned long)busy, (unsigned long)union_ns,
		       (long)((int64_t)busy - (int64_t)union_ns));
		rc = 1;
	}
	printf("stress: %d jobs, busy_ns=%lu, consumed slot union=%lu, reader bounded %s\n",
	       STRESS_PRODUCERS * STRESS_ITERS, (unsigned long)busy,
	       (unsigned long)union_ns, rc ? "FAIL" : "ok");
	return rc;
}

/* ---- Root-cause proof: stale submit sample straddling a boundary. ----
 *
 * A caller samples submit_ns, then runs the ticket fetch and the
 * inflight transition a few instructions later. If a period closes
 * and the next one opens inside that window, the sample predates the
 * period the submission is actually counted in. The implementation
 * counts from the latch (never backward: latch = max(submit_ns,
 * max_end)); a reference union built from raw caller samples
 * overcounts by exactly the sample-to-latch gap.
 */
static int test_stale_submit_sample(void)
{
	struct fixture f;
	uint64_t a, d, c;
	uint64_t busy;

	fx_init(&f);
	a = ane_stats_begin(&f.ctrs, &f.ring, 1000ull, 1);
	ane_stats_complete(&f.ctrs, &f.ring, a, 2000ull, 0, 0);
	d = ane_stats_begin(&f.ctrs, &f.ring, 2010ull, 1);
	c = ane_stats_begin(&f.ctrs, &f.ring, 1995ull, 1);
	ane_stats_complete(&f.ctrs, &f.ring, c, 2400ull, 0, 0);
	ane_stats_complete(&f.ctrs, &f.ring, d, 2600ull, 0, 0);
	busy = ane_stats_atomic64_read(&f.ctrs.busy_ns);
	uint64_t consumed = 1000ull + (2600ull - 2010ull);
	uint64_t naive = 1600ull;
	int ok = busy == consumed && busy != naive;
	printf("stale_sample: busy=%lu consumed_union=%lu naive_union=%lu (%s)\n",
	       (unsigned long)busy, (unsigned long)consumed,
	       (unsigned long)naive,
	       ok ? "implementation counts from the latch; the naive sample union overcounts"
		  : "MISMATCH");
	return ok ? 0 : 1;
}

/* ---- Model check: every interleaving of 3 two-op jobs against the
 * consumed slot-union property. ---- */
static uint64_t mc_union_slots(struct fixture *f, int njobs)
{
	uint64_t union_ns = 0, lo = 0, hi = 0;
	int have = 0;
	int i;

	for (i = 0; i < njobs; i++) {
		struct ane_stats_ring_entry *e = &f->ring.slots[i];
		uint64_t s = ane_stats_atomic64_read(&e->start_ns);
		uint64_t t = ane_stats_atomic64_read(&e->end_ns);

		if (t < s)
			t = s;
		if (!have || s > hi) {
			union_ns += hi - lo;
			lo = s;
			hi = t;
			have = 1;
		} else if (t > hi) {
			hi = t;
		}
	}
	if (have)
		union_ns += hi - lo;
	return union_ns;
}

static int test_model_check(void)
{
	/* 3 jobs, all overlapping the others somewhere. */
	struct mc_job {
		uint64_t s;
		uint64_t e;
	} jobs[3] = {
		{ 100ull, 200ull },
		{ 150ull, 260ull },
		{ 120ull, 180ull },
	};
	int order[6] = { 0, 1, 2, 3, 4, 5 };
	int fails = 0, runs = 0;
	int i, j, k, l, m, n;

	for (i = 0; i < 6; i++)
	for (j = 0; j < 6; j++) { if (j == i) continue;
	for (k = 0; k < 6; k++) { if (k == i || k == j) continue;
	for (l = 0; l < 6; l++) { if (l == i || l == j || l == k) continue;
	for (m = 0; m < 6; m++) { if (m == i || m == j || m == k || m == l) continue;
	for (n = 0; n < 6; n++) { if (n == i || n == j || n == k || n == l || n == m) continue;
		int seq[6];
		int op, seen[3] = { 0, 0, 0 };
		int valid = 1;
		struct fixture f;

		seq[0] = order[i]; seq[1] = order[j]; seq[2] = order[k];
		seq[3] = order[l]; seq[4] = order[m]; seq[5] = order[n];
		for (op = 0; op < 6 && valid; op++) {
			int job = seq[op] / 2;

			if (seq[op] & 1) {
				if (!seen[job])
					valid = 0;
			} else {
				seen[job] = 1;
			}
		}
		if (!valid)
			continue;
		runs++;
		fx_init(&f);
		{
			uint64_t ticket[3] = { 0, 0, 0 };
			uint64_t busy;

			for (op = 0; op < 6; op++) {
				int job = seq[op] / 2;

				if (!(seq[op] & 1))
					ticket[job] = ane_stats_begin(
						&f.ctrs, &f.ring,
						jobs[job].s, 1);
				else
					ane_stats_complete(
						&f.ctrs, &f.ring,
						ticket[job], jobs[job].e,
						0, 0);
			}
			busy = ane_stats_atomic64_read(&f.ctrs.busy_ns);
			if (busy != mc_union_slots(&f, 3))
				fails++;
		}
	}}}}}
	printf("model_check: %d interleavings, %d failures (busy == consumed slot union)\n",
	       runs, fails);
	return fails ? 1 : 0;
}

/* ---- Begin-internal step model: the drain-inside-begin race. ----
 *
 * The single-threaded real begin() cannot interleave, so this harness
 * splits begin() into its atomic steps and lets the interleaving
 * driver run the previous period's drain between them. PRE latches
 * from max_end before the 0->TRANS cmpxchg (the pre-fix order); POST
 * latches after winning it (the fix). Times: A = [1000,2000],
 * B = [1500,2500] - overlapping windows, so the union is 1500 and any
 * period overlap double-counts toward 2000.
 */
enum mb_step {
	MB_B_LATCH,   /* read max_end for the latch candidate */
	MB_B_TRANS,   /* win 0->TRANS, write latch, inflight = 1 */
	MB_B_FEED,    /* completion: feed end into max_end */
	MB_B_DRAIN,   /* completion: fold period A, inflight = 0 */
};

static int mb_latch_post; /* 0 = pre-fix order, 1 = post-fix order */

static uint64_t mb_busy;

static void mb_run(const int *steps, int n, int post)
{
	struct fixture f;

	fx_init(&f);
	mb_busy = 0;
	mb_latch_post = post;
	for (int i = 0; i < n; i++) {
		uint64_t s;

		switch (steps[i]) {
		case MB_B_LATCH:
			/* Pre-fix reads the candidate here; post-fix
			 * ignores it (re-read after the transition). */
			break;
		case MB_B_TRANS:
			if (post)
				s = 2000ull; /* sees A's feed */
			else
				s = 0ull;    /* pre-drain max_end */
			if (s < 1500ull)
				s = 1500ull;
			ane_stats_atomic64_set_release(&f.ctrs.last_busy_end, s);
			ane_stats_atomic_set_release(&f.ctrs.inflight, 1u);
			break;
		case MB_B_FEED:
			ane_stats_atomic64_max(2000ull, &f.ctrs.max_end);
			break;
		case MB_B_DRAIN:
			ane_stats_atomic64_add(2000ull - 1000ull, &f.ctrs.busy_ns);
			break;
		}
	}
	/* B's completion folds [latch, 2500]. */
	{
		uint64_t l = ane_stats_atomic64_read(&f.ctrs.last_busy_end);

		mb_busy = ane_stats_atomic64_read(&f.ctrs.busy_ns) +
			  (2500ull - l);
	}
}

static int test_drain_inside_begin(void)
{
	/* A drains between B's latch read and B's transition. */
	const int race[] = { MB_B_LATCH, MB_B_FEED, MB_B_DRAIN, MB_B_TRANS };
	const int nobrace[] = { MB_B_FEED, MB_B_DRAIN, MB_B_LATCH, MB_B_TRANS };
	uint64_t pre, post;

	mb_run(race, 4, 0);
	pre = mb_busy;
	mb_run(race, 4, 1);
	post = mb_busy;
	/* Serialized control: no drain inside the window. */
	mb_run(nobrace, 4, 0);
	int ok = pre == 2000ull && post == 1500ull;
	printf("drain_inside_begin: pre_fix_busy=%lu (double-counts; union 1500) post_fix_busy=%lu %s\n",
	       (unsigned long)pre, (unsigned long)post,
	       ok ? "fixed" : "MISMATCH");
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
	rc |= test_emit_live_tail();
	rc |= test_period_gap_no_accrual();
	rc |= test_fold_only_at_drain();
	rc |= test_stale_submit_sample();
	rc |= test_drain_inside_begin();
	rc |= test_model_check();
	rc |= test_randomized_stress();
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