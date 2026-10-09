/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Keep-warm tickle decision — the userspace-testable core.
 *
 * ane_t6021_keepwarm_plan() is pure: it reads only its input struct and
 * writes only its output struct, so the host unit test
 * (tests/keepwarm_test.c) exercises the exact decision the kthread runs.
 * It uses no kernel-only types beyond bool and long long, and performs
 * no I/O. The thread applies the answer; this header only decides it.
 */
#ifndef __ANE_T6021_KEEPWARM_H__
#define __ANE_T6021_KEEPWARM_H__

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#endif

#define ANE_T6021_KEEPWARM_MIN_US	20
#define ANE_T6021_KEEPWARM_TAIL_MAX_US	20000

struct ane_t6021_keepwarm_in {
	long long keepwarm_us;		/* 0 = off */
	long long keepwarm_tail_us;
	long long last_done_ns;		/* last CALL completion; 0 = none yet */
	long long last_tickle_ns;
	long long now_ns;
	bool lock_free;			/* mutex_trylock(fw_lock) succeeded */
	bool device_ready;		/* held, chman_ok, rings mapped, not quarantined */
};

struct ane_t6021_keepwarm_out {
	bool tickle;
	long long sleep_us;		/* 0 = wait for the next completion or stop */
};

/* Tick interval with the 20 us floor. */
static inline long long
ane_t6021_keepwarm_interval_us(long long keepwarm_us)
{
	return keepwarm_us < ANE_T6021_KEEPWARM_MIN_US ?
	       ANE_T6021_KEEPWARM_MIN_US : keepwarm_us;
}

/* Tail with the 20000 us hard cap. */
static inline long long
ane_t6021_keepwarm_cap_us(long long keepwarm_tail_us)
{
	return keepwarm_tail_us > ANE_T6021_KEEPWARM_TAIL_MAX_US ?
	       ANE_T6021_KEEPWARM_TAIL_MAX_US : keepwarm_tail_us;
}

static inline void
ane_t6021_keepwarm_plan(const struct ane_t6021_keepwarm_in *in,
			struct ane_t6021_keepwarm_out *out)
{
	long long interval = ane_t6021_keepwarm_interval_us(in->keepwarm_us);
	long long tail = ane_t6021_keepwarm_cap_us(in->keepwarm_tail_us);
	long long remaining;

	out->tickle = false;
	out->sleep_us = 0;	/* 0 = wait for the next completion or stop */

	/* Off: the knob is the whole feature. */
	if (in->keepwarm_us == 0)
		return;

	/* Tail expired (or no completion ever recorded): idle until the
	 * next completion wakes the thread. last_done_ns == 0 puts every
	 * real clock far past the tail.
	 */
	if (in->now_ns - in->last_done_ns >= tail * 1000ll)
		return;

	/* Inside the tail, but the interval has not elapsed since the
	 * last tickle: sleep the remaining time, floored at 20 us, so a
	 * long interval does not re-decide at the floor cadence.
	 */
	if (in->now_ns - in->last_tickle_ns < interval * 1000ll) {
		remaining = interval -
			    (in->now_ns - in->last_tickle_ns) / 1000ll;
		if (remaining < ANE_T6021_KEEPWARM_MIN_US)
			remaining = ANE_T6021_KEEPWARM_MIN_US;
		out->sleep_us = remaining;
		return;
	}

	/* A call in flight holds the lock and already keeps the
	 * firmware warm: back off one interval.
	 */
	if (!in->lock_free) {
		out->sleep_us = interval;
		return;
	}

	/* No registered device, or the transport is unproven or the
	 * device quarantined: the doorbell must not be rung.
	 */
	if (!in->device_ready) {
		out->sleep_us = interval;
		return;
	}

	out->tickle = true;
	out->sleep_us = interval;
}

#endif /* __ANE_T6021_KEEPWARM_H__ */
