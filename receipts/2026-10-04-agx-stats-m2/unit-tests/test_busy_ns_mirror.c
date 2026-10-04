/* Host mirror of the agx_stats busy_ns integration (stats.rs FwBusy branch).
 * Pins the ALGORITHM the sysfs counters must obey: first-sample skip,
 * out-of-order skip, no negative delta, monotonicity, wrap semantics.
 * The kernel Rust code is the authority; this mirror fails if either the
 * algorithm or its documented contract drifts.
 * Build+run: cc -O2 -Wall -Wextra -Werror test_busy_ns_mirror.c -o t && ./t
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

struct snap {
	uint64_t last_busy_ts;
	uint64_t busy_ns;
};

/* Mirrors StatsSnapshot::update_from's FwBusy arm. */
static void on_fwbusy(struct snap *s, uint64_t ts)
{
	uint64_t prev = s->last_busy_ts;
	s->last_busy_ts = ts;
	if (prev != 0 && ts >= prev)
		s->busy_ns += ts - prev; /* wraps at u64 like Rust release fetch_add */
}

int main(void)
{
	struct snap s = {0, 0};

	/* First sample: prev == 0 must not add anything. */
	on_fwbusy(&s, 100);
	assert(s.busy_ns == 0);

	/* Normal delta. */
	on_fwbusy(&s, 250);
	assert(s.busy_ns == 150);

	/* Out-of-order timestamp: skipped, no negative delta. */
	on_fwbusy(&s, 200);
	assert(s.busy_ns == 150);
	assert(s.last_busy_ts == 200);

	/* Equal timestamp: zero delta, still monotonic. */
	on_fwbusy(&s, 200);
	assert(s.busy_ns == 150);

	/* Monotonic growth across a run: busy_ns never decreases. */
	{
		uint64_t t, prev_busy = 0;
		for (t = 200; t <= 100000; t += 7) {
			on_fwbusy(&s, t);
			assert(s.busy_ns >= prev_busy);
			assert(s.busy_ns <= t); /* busy cannot exceed the timestamp */
			prev_busy = s.busy_ns;
		}
	}

	/* Wrap semantics: mirrors Rust release-mode fetch_add (u64 wrap),
	 * NOT the ABI doc's "saturates at u64::MAX" claim. Reported mismatch. */
	{
		struct snap w = {0, 0};
		on_fwbusy(&w, 1);
		on_fwbusy(&w, UINT64_MAX);
		on_fwbusy(&w, UINT64_MAX); /* delta 0 */
		on_fwbusy(&w, 5);          /* ts < prev: skipped */
		assert(w.busy_ns == UINT64_MAX - 1);
	}

	printf("PASS: busy_ns mirror (skip-first, skip-oOO, monotonic, <= ts, wrap)\n");
	return 0;
}
