// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Host unit test for the pipelined-CALL completion demux
 * (ane_t6021_pipeline.h). Plain C, no frameworks:
 *
 *   gcc -Wall -Wextra -Werror -O2 -I.. -o pipeline_test pipeline_test.c \
 *       -lpthread
 *   ./pipeline_test
 *
 * Five groups over the pure core, then a two-thread simulation of two
 * armed callers against a shared fake ring. Each group is the
 * negative-control target of one mutation: deleting (disabling) the
 * guard the group asserts must flip this test to FAIL with exit 1.
 *   m1: released() drops the (prog,proc) key match        -> T3
 *   m2: finish() drops the owed guard (finished<submitted) -> T4
 *   m3: released() drops the ticket position (>= ticket)   -> T2
 *   m4: submit() drops the table-full bail                 -> T5
 *   m5: released() drops the comparison entirely           -> T1
 * Mutation runs are recorded in the notebook entry, not here.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

#include "ane_t6021_pipeline.h"

static int failures;

static void check(const char *name, bool cond)
{
	if (!cond) {
		printf("FAIL %s\n", name);
		failures++;
	}
}

struct sim {
	struct ane_t6021_pipe_demux d;
	pthread_mutex_t lock;
	/* Fake ring: the drain credits every event it holds.
	 */
	pthread_mutex_t ring_lock;
	u32 ev[16][2];
	int n_ev;
	atomic_int released[2];	/* release observations per caller */
};

struct sim_args {
	struct sim *s;
	int me;			/* 0 -> proc 100, 1 -> proc 200 */
};

static u64 submit_one(struct sim *s, u32 prog, u32 proc)
{
	u64 ticket = 0;
	int ret;

	pthread_mutex_lock(&s->lock);
	ret = ane_t6021_pipe_submit(&s->d, prog, proc, &ticket);
	pthread_mutex_unlock(&s->lock);
	return ret ? 0 : ticket;
}

static void push_event(struct sim *s, u32 prog, u32 proc)
{
	pthread_mutex_lock(&s->ring_lock);
	s->ev[s->n_ev][0] = prog;
	s->ev[s->n_ev][1] = proc;
	s->n_ev++;
	pthread_mutex_unlock(&s->ring_lock);
}

/* The driver's drain: one pass over the ring under the ring lock,
 * crediting every finish into the demux.
 */
static void drain_ring(struct sim *s)
{
	pthread_mutex_lock(&s->ring_lock);
	pthread_mutex_lock(&s->lock);
	while (s->n_ev) {
		u32 prog = s->ev[s->n_ev - 1][0];
		u32 proc = s->ev[s->n_ev - 1][1];

		s->n_ev--;
		ane_t6021_pipe_finish(&s->d, prog, proc);
	}
	pthread_mutex_unlock(&s->lock);
	pthread_mutex_unlock(&s->ring_lock);
}

static bool released_one(struct sim *s, u32 prog, u32 proc,
			 u64 ticket)
{
	bool r;

	pthread_mutex_lock(&s->lock);
	r = ane_t6021_pipe_released(&s->d, prog, proc, ticket);
	pthread_mutex_unlock(&s->lock);
	return r;
}

static unsigned int outstanding_one(struct sim *s)
{
	unsigned int v;

	pthread_mutex_lock(&s->lock);
	v = ane_t6021_pipe_outstanding(&s->d);
	pthread_mutex_unlock(&s->lock);
	return v;
}

/* Caller body: submit, poll (drain + check) until released, then count
 * the release exactly once. A bound on the poll keeps a mutation that
 * hangs (never releases) a FAIL, not a hang.
 */
static void *caller(void *p)
{
	struct sim_args *sa = p;
	struct sim *s = sa->s;
	u32 proc = sa->me ? 200 : 100;
	u64 ticket = submit_one(s, 7, proc);

	if (!ticket)
		return (void *)1;
	for (int i = 0; i < 10000000; i++) {
		drain_ring(s);
		if (released_one(s, 7, proc, ticket))
			break;
	}
	if (!released_one(s, 7, proc, ticket))
		return (void *)1;
	if (atomic_fetch_add(&s->released[sa->me], 1) != 0)
		return (void *)1;
	return NULL;
}

int main(void)
{
	struct ane_t6021_pipe_demux d;
	u64 t1, t2;
	int ret;

	/* T1 sequential: submit, finish, released. (Mutation m5: the
	 * comparison in released() disabled.)
	 */
	d = (struct ane_t6021_pipe_demux){0};
	ret = ane_t6021_pipe_submit(&d, 7, 100, &t1);
	check("T1a submit takes ticket 1", ret == 0 && t1 == 1);
	check("T1b not released before the event",
	      !ane_t6021_pipe_released(&d, 7, 100, t1));
	ane_t6021_pipe_finish(&d, 7, 100);
	check("T1c released by own event",
	      ane_t6021_pipe_released(&d, 7, 100, t1));
	check("T1d in_flight back to 0", d.in_flight == 0);

	/* T2 same-key FIFO: two tickets, the first event releases only
	 * t1. (Mutation m3: the ticket-position compare disabled — t2
	 * would release on the first event.)
	 */
	d = (struct ane_t6021_pipe_demux){0};
	ane_t6021_pipe_submit(&d, 7, 100, &t1);
	ane_t6021_pipe_submit(&d, 7, 100, &t2);
	check("T2a tickets are positional", t1 == 1 && t2 == 2);
	ane_t6021_pipe_finish(&d, 7, 100);
	check("T2b first event releases t1",
	      ane_t6021_pipe_released(&d, 7, 100, t1));
	check("T2c first event does not release t2",
	      !ane_t6021_pipe_released(&d, 7, 100, t2));
	ane_t6021_pipe_finish(&d, 7, 100);
	check("T2d second event releases t2",
	      ane_t6021_pipe_released(&d, 7, 100, t2));

	/* T3 wrong key: an event for (8,200) releases nothing of (7,100)
	 * either before or after (7,100) is finished, and a key that
	 * never submitted has no ticket. (Mutation m1: the key match in
	 * released() deleted — the finished (7,100) key would release a
	 * query for (8,200).
	 */
	d = (struct ane_t6021_pipe_demux){0};
	ane_t6021_pipe_submit(&d, 7, 100, &t1);
	ane_t6021_pipe_finish(&d, 8, 200);
	check("T3a wrong-key event releases nothing",
	      !ane_t6021_pipe_released(&d, 7, 100, t1));
	check("T3b wrong-key event is owed nowhere",
	      d.in_flight == 1 && ane_t6021_pipe_outstanding(&d) == 1);
	ane_t6021_pipe_finish(&d, 7, 100);
	check("T3c own event releases after a wrong one",
	      ane_t6021_pipe_released(&d, 7, 100, t1));
	check("T3d no release for a never-submitted key",
	      !ane_t6021_pipe_released(&d, 8, 200, 1));

	/* T4 stale events: a finish with nothing owed must credit
	 * nothing — not the zeroed slot, not a later ticket.
	 * (Mutation m2: the owed guard in finish() deleted — the
	 * unsubmitted (0,0) event would underflow in_flight and the
	 * later ticket would release early.
	 */
	d = (struct ane_t6021_pipe_demux){0};
	ane_t6021_pipe_finish(&d, 0, 0);
	check("T4a unsubmitted (0,0) event credits nothing",
	      ane_t6021_pipe_outstanding(&d) == 0);
	ane_t6021_pipe_submit(&d, 0, 0, &t1);
	check("T4b unsubmitted event cannot satisfy the first ticket",
	      !ane_t6021_pipe_released(&d, 0, 0, t1));
	ane_t6021_pipe_finish(&d, 0, 0);
	check("T4c own event satisfies the first ticket",
	      ane_t6021_pipe_released(&d, 0, 0, t1));
	ane_t6021_pipe_finish(&d, 0, 0);
	ane_t6021_pipe_submit(&d, 0, 0, &t2);
	check("T4d duplicate event cannot satisfy the next ticket",
	      !ane_t6021_pipe_released(&d, 0, 0, t2));

	/* T5 table full: 16 distinct keys submit, the 17th is rejected,
	 * finished keys keep their slots (no eviction).
	 * (Mutation m4: the table-full bail in submit() deleted.)
	 */
	d = (struct ane_t6021_pipe_demux){0};
	ret = 1;
	for (u32 i = 0; i < ANE_T6021_PIPE_KEYS; i++)
		ret &= ane_t6021_pipe_submit(&d, 1000 + i, i, &t1) == 0;
	check("T5a all 16 keys submit", ret == 1);
	check("T5b 17th key is rejected",
	      ane_t6021_pipe_submit(&d, 9999, 9999, &t1) != 0);
	check("T5c outstanding counts all 16",
	      ane_t6021_pipe_outstanding(&d) == ANE_T6021_PIPE_KEYS);
	for (u32 i = 0; i < ANE_T6021_PIPE_KEYS; i++)
		ane_t6021_pipe_finish(&d, 1000 + i, i);
	check("T5d all finished: outstanding 0",
	      ane_t6021_pipe_outstanding(&d) == 0);
	check("T5e finished keys still hold their slots",
	      ane_t6021_pipe_submit(&d, 9999, 9999, &t1) != 0);

	/* T6 simulation: two callers on two process ids (separate keys),
	 * events drained out of order (B before A) and in order. Each
	 * caller must release exactly once, only through its own event;
	 * both tickets exist before any event is drained.
	 */
	for (int order = 0; order < 2; order++) {
		struct sim s = {0};
		struct sim_args sa0 = { &s, 0 }, sa1 = { &s, 1 };
		pthread_t a, b;
		void *ra = (void *)1, *rb = (void *)1;
		int cr, jr;

		cr = pthread_mutex_init(&s.lock, NULL);
		cr |= pthread_mutex_init(&s.ring_lock, NULL);
		atomic_init(&s.released[0], 0);
		atomic_init(&s.released[1], 0);
		cr |= pthread_create(&a, NULL, caller, &sa0);
		cr |= pthread_create(&b, NULL, caller, &sa1);
		if (!cr) {
			/* Both tickets before any event. */
			for (int i = 0; i < 100000000 &&
			     outstanding_one(&s) < 2; i++)
				;
			if (order == 0) {
				push_event(&s, 7, 200);
				drain_ring(&s);
				push_event(&s, 7, 100);
				drain_ring(&s);
			} else {
				push_event(&s, 7, 100);
				push_event(&s, 7, 200);
				drain_ring(&s);
			}
			jr = pthread_join(a, &ra);
			jr |= pthread_join(b, &rb);
		}
		check(order == 0 ?
		      "T6a out-of-order events: each caller released once" :
		      "T6b in-order events: each caller released once",
		      cr == 0 && jr == 0 && !ra && !rb &&
		      atomic_load(&s.released[0]) == 1 &&
		      atomic_load(&s.released[1]) == 1);
	}

	if (failures) {
		printf("%d failure(s)\n", failures);
		return 1;
	}
	printf("pipeline_test: all checks pass\n");
	return 0;
}
