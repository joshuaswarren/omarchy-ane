/* Host mirror of the agx_stats busy_ns integration (channel.rs Utilization
 * arm, utilization-weighted producer over base-clock ticks). Pins the
 * ALGORITHM the sysfs counter must obey: first-window skip, out-of-order
 * skip, strict monotonicity, wall-time bound (a window contributes at most
 * its full duration), weighted accumulation (util 100 => full delta, util 50
 * => half), the 24 MHz tick -> ns conversion, and rate invariance (a
 * different base_clock_hz with proportionally scaled ticks yields the same
 * ns). The kernel Rust code is the authority; this mirror fails if either
 * the algorithm or its documented contract drifts.
 * Build+run: cc -O2 -Wall -Wextra -Werror test_busy_ns_mirror.c -o t && ./t
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

struct chan {
	uint64_t ts_hz; /* firmware timestamp rate, Hz */
	uint64_t last_util_ts; /* 0 = no window seen yet */
	uint64_t busy_ns;
};

/* Mirrors the StatsChannel::poll Utilization arm (u128 intermediate in the
 * kernel; unsigned long long is sufficient for these vectors). */
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
	if (c->last_util_ts != 0 && ts > c->last_util_ts) {
		unsigned long long busy =
			((unsigned long long)(ts - c->last_util_ts) * util *
			 1000000000ULL) /
			(100ULL * c->ts_hz);
		c->busy_ns += busy;
	}
	c->last_util_ts = ts;
}

int main(void)
{
	struct chan c = { 24000000, 0, 0 };

	/* First window: no previous timestamp, nothing added. */
	on_utilization(&c, 24000000, 100, 100, 100, 100);
	assert(c.busy_ns == 0);

	/* util 100 across 24,000 ticks (= 1 ms at 24 MHz): 1e6 ns. */
	on_utilization(&c, 24024000, 100, 100, 100, 100);
	assert(c.busy_ns == 1000000);

	/* util 50 across 1 ms (busiest subqueue = 50): 5e5 ns. */
	on_utilization(&c, 24048000, 50, 0, 0, 0);
	assert(c.busy_ns == 1500000);

	/* Out-of-order timestamp: skipped entirely. */
	on_utilization(&c, 24036000, 100, 100, 100, 100);
	assert(c.busy_ns == 1500000);
	assert(c.last_util_ts == 24036000); /* window still updates the clock */

	/* Equal timestamp: zero duration, no change. */
	on_utilization(&c, 24036000, 100, 100, 100, 100);
	assert(c.busy_ns == 1500000);

	/* Monotonicity and the wall bound across a run. */
	{
		uint64_t t, prev_busy = 0;
		uint32_t pattern[] = { 100, 51, 3, 100, 0, 77 };
		for (t = 24048000; t <= 24048000 + 24000000ULL; t += 24000) {
			uint32_t u = pattern[((t - 24048000) / 24000) % 6];
			on_utilization(&c, t, u, u, u, u);
			assert(c.busy_ns >= prev_busy);
			assert(c.busy_ns <= (t - 24000000) * 1000000000ULL / 24000000ULL);
			prev_busy = c.busy_ns;
		}
	}

	/* 100% over exactly 1 s of 24 MHz ticks: 1e9 ns. */
	{
		struct chan s = { 24000000, 0, 0 };
		uint64_t t;
		on_utilization(&s, 0, 100, 100, 100, 100);
		for (t = 24000000; t <= 48000000; t += 24000000)
			on_utilization(&s, t, 100, 100, 100, 100);
		assert(s.busy_ns == 1000000000ULL);
	}

	/* 50% over exactly 1 s of 24 MHz ticks: 5e8 ns. */
	{
		struct chan h = { 24000000, 0, 0 };
		uint64_t t;
		on_utilization(&h, 0, 50, 0, 0, 0);
		for (t = 24000000; t <= 48000000; t += 24000000)
			on_utilization(&h, t, 50, 0, 0, 0);
		assert(h.busy_ns == 500000000ULL);
	}

	/* Rate invariance: a 48 MHz base clock with doubled tick counts over
	 * the same wall second yields the same ns as 24 MHz. */
	{
		struct chan a = { 24000000, 0, 0 };
		struct chan b = { 48000000, 0, 0 };
		uint64_t t;
		on_utilization(&a, 0, 70, 0, 0, 0);
		on_utilization(&b, 0, 70, 0, 0, 0);
		for (t = 24000000; t <= 48000000; t += 24000000)
			on_utilization(&a, t, 70, 0, 0, 0);
		for (t = 48000000; t <= 96000000; t += 48000000)
			on_utilization(&b, t, 70, 0, 0, 0);
		assert(a.busy_ns == 700000000ULL);
		assert(b.busy_ns == 700000000ULL);
	}

	/* Saturated matmul shape in 24 MHz ticks: windows every 0.1 s
	 * (2,400,000 ticks) at util 100 for 30 s => 30e9 ns. */
	{
		struct chan m = { 24000000, 0, 0 };
		uint64_t t, windows = 0;
		/* First window at 2.4 s is the sentinel-skipped seed. */
		on_utilization(&m, 2400000, 100, 100, 100, 100);
		for (t = 4800000; t <= 722400000; t += 2400000) {
			on_utilization(&m, t, 100, 100, 100, 100);
			windows++;
		}
		assert(windows == 300);
		assert(m.busy_ns == 30000000000ULL);
	}

	printf("PASS: busy_ns mirror (first-skip, oOO-skip, monotonic, <= wall, weighted, tick conversion, rate invariance)\n");
	return 0;
}
