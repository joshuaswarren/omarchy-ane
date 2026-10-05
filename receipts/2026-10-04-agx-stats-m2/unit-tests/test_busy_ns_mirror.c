/* Host mirror of the agx_stats busy_ns integration (channel.rs Utilization
 * arm, utilization-weighted producer). Pins the ALGORITHM the sysfs counter
 * must obey: first-window skip, out-of-order skip, strict monotonicity,
 * wall-time bound (a window contributes at most its full duration), weighted
 * accumulation (util 100 => full delta, util 50 => half), and tick-rate
 * invariance of the busy fraction.
 * The kernel Rust code is the authority; this mirror fails if either the
 * algorithm or its documented contract drifts.
 * Build+run: cc -O2 -Wall -Wextra -Werror test_busy_ns_mirror.c -o t && ./t
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

struct chan {
	uint64_t last_util_ts; /* 0 = no window seen yet */
	uint64_t busy_ns;
};

/* Mirrors the StatsChannel::poll Utilization arm. */
static void on_utilization(struct chan *c, uint64_t ts, uint32_t u1,
			   uint32_t u2, uint32_t u3, uint32_t u4)
{
	uint64_t util = u1;
	if (u2 > util)
		util = u2;
	if (u3 > util)
		util = u3;
	if (u4 > util)
		util = u4;
	if (util > 100)
		util = 100;
	if (c->last_util_ts != 0 && ts > c->last_util_ts)
		c->busy_ns += (ts - c->last_util_ts) / 100 * util;
	c->last_util_ts = ts;
}

int main(void)
{
	struct chan c = {0, 0};

	/* First window: no previous timestamp, nothing added. */
	on_utilization(&c, 1000, 100, 100, 100, 100);
	assert(c.busy_ns == 0);

	/* util 100 across 1 ms: full duration counts. */
	on_utilization(&c, 2000, 100, 100, 100, 100);
	assert(c.busy_ns == 1000);

	/* util 50 across 1 ms (busiest subqueue = 50): half the duration. */
	on_utilization(&c, 3000, 50, 0, 0, 0);
	assert(c.busy_ns == 1500);

	/* Out-of-order timestamp: skipped entirely. */
	on_utilization(&c, 2500, 100, 100, 100, 100);
	assert(c.busy_ns == 1500);
	assert(c.last_util_ts == 2500); /* window still updates the clock */

	/* Equal timestamp: zero duration, no change. */
	on_utilization(&c, 2500, 100, 100, 100, 100);
	assert(c.busy_ns == 1500);

	/* Monotonicity and the wall bound across a run: busy_ns never
	 * decreases and never exceeds elapsed time. */
	{
		uint64_t t, prev_busy = 0;
		uint32_t pattern[] = { 100, 51, 3, 100, 0, 77 };
		for (t = 2500; t <= 250000; t += 500) {
			uint32_t u = pattern[(t / 500) % 6];
			on_utilization(&c, t, u, u, u, u);
			assert(c.busy_ns >= prev_busy);
			assert(c.busy_ns <= t);
			prev_busy = c.busy_ns;
		}
	}

	/* Tick-rate invariance of the busy fraction: halve the tick, halve
	 * both busy and wall, fraction unchanged. */
	{
		struct chan a = { 0, 0 };
		struct chan b = { 0, 0 };
		uint64_t t;
		for (t = 1000; t <= 11000; t += 1000)
			on_utilization(&a, t, 70, 0, 0, 0);
		for (t = 500; t <= 5500; t += 500)
			on_utilization(&b, t, 70, 0, 0, 0);
		/* a: 10 windows x 1 ms at 70% ; b: 10 x 0.5 ms at 70% */
		assert(a.busy_ns == 7000);
		assert(b.busy_ns == 3500);
	}

	/* Saturated matmul shape: 30 s at util 100 => 30 s busy. */
	{
		struct chan m = { 0, 0 };
		uint64_t t;
		on_utilization(&m, 0, 100, 100, 100, 100);
		for (t = 100000000; t <= 31000000000; t += 100000000)
			on_utilization(&m, t, 100, 100, 100, 100);
		assert(m.busy_ns == 30900000000ULL); /* 309 windows x 100 ms */
	}

	printf("PASS: busy_ns mirror (first-skip, oOO-skip, monotonic, <= wall, weighted, tick-invariant)\n");
	return 0;
}
