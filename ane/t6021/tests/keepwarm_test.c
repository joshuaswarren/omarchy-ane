// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Host unit test for the keep-warm decision core (ane_t6021_keepwarm.h).
 * Plain C, no frameworks:
 *
 *   gcc -Wall -Wextra -Werror -O2 -I.. -o keepwarm_test keepwarm_test.c
 *   ./keepwarm_test
 *
 * Six groups, one per decision check. Each group is the negative-control
 * target of one mutation: deleting (disabling) the check the group
 * asserts must flip this test to FAIL with exit 1. Mutation runs are
 * recorded in the notebook entry, not here.
 */
#include <stdio.h>

#include "ane_t6021_keepwarm.h"

static int failures;

static void check(const char *name, bool cond)
{
	if (!cond) {
		printf("FAIL %s\n", name);
		failures++;
	}
}

#define US(x) ((long long)(x) * 1000ll)

int main(void)
{
	struct ane_t6021_keepwarm_in in;
	struct ane_t6021_keepwarm_out out;
	const long long now = 1000000000000ll;

	/* T1 off: keepwarm_us = 0 -> no tickle, wait for a completion. */
	in = (struct ane_t6021_keepwarm_in){
		.keepwarm_us = 0, .keepwarm_tail_us = 2000,
		.last_done_ns = now - US(1), .last_tickle_ns = 0,
		.now_ns = now, .lock_free = true, .device_ready = true,
	};
	ane_t6021_keepwarm_plan(&in, &out);
	check("T1a off: no tickle", !out.tickle);
	check("T1b off: waits for completion", out.sleep_us == 0);

	/* T2 tail: past the tail -> idle; no completion ever -> idle. */
	in = (struct ane_t6021_keepwarm_in){
		.keepwarm_us = 50, .keepwarm_tail_us = 2000,
		.last_done_ns = now - US(2500), .last_tickle_ns = 0,
		.now_ns = now, .lock_free = true, .device_ready = true,
	};
	ane_t6021_keepwarm_plan(&in, &out);
	check("T2a past tail: no tickle", !out.tickle);
	check("T2b past tail: waits for completion", out.sleep_us == 0);
	in.last_done_ns = 0;
	ane_t6021_keepwarm_plan(&in, &out);
	check("T2c no completion yet: no tickle", !out.tickle);

	/* T3 lock held: due, inside the tail, ready, but the firmware
	 * lock is held by a call in flight.
	 */
	in = (struct ane_t6021_keepwarm_in){
		.keepwarm_us = 50, .keepwarm_tail_us = 2000,
		.last_done_ns = now - US(500), .last_tickle_ns = now - US(60),
		.now_ns = now, .lock_free = false, .device_ready = true,
	};
	ane_t6021_keepwarm_plan(&in, &out);
	check("T3a lock held: no tickle", !out.tickle);
	check("T3b lock held: backs off one interval", out.sleep_us == 50);

	/* T4 tail cap: an input far over 20000 us behaves as 20000 us.
	 */
	in = (struct ane_t6021_keepwarm_in){
		.keepwarm_us = 50, .keepwarm_tail_us = 999999,
		.last_done_ns = now - US(25000), .last_tickle_ns = 0,
		.now_ns = now, .lock_free = true, .device_ready = true,
	};
	ane_t6021_keepwarm_plan(&in, &out);
	check("T4a tail cap: 25 ms is past the capped tail", !out.tickle);
	check("T4b tail cap: waits for completion", out.sleep_us == 0);

	/* T5 min interval: the interval floors at 20 us, on both the
	 * not-due decision and the post-tickle sleep.
	 */
	in = (struct ane_t6021_keepwarm_in){
		.keepwarm_us = 5, .keepwarm_tail_us = 2000,
		.last_done_ns = now - US(500), .last_tickle_ns = now - US(5),
		.now_ns = now, .lock_free = true, .device_ready = true,
	};
	ane_t6021_keepwarm_plan(&in, &out);
	check("T5a below floor: interval not elapsed, no tickle",
	      !out.tickle);
	check("T5b below floor: re-decide sleep >= 20 us",
	      out.sleep_us >= 20);
	in = (struct ane_t6021_keepwarm_in){
		.keepwarm_us = 5, .keepwarm_tail_us = 2000,
		.last_done_ns = now - US(500), .last_tickle_ns = now - US(100),
		.now_ns = now, .lock_free = true, .device_ready = true,
	};
	ane_t6021_keepwarm_plan(&in, &out);
	check("T5c tickle sleep floored at 20 us", out.sleep_us == 20);

	/* T5d not due: the sleep is the remaining interval, not the
	 * floor (keepwarm_us 100, 30 us since the last tickle).
	 */
	in = (struct ane_t6021_keepwarm_in){
		.keepwarm_us = 100, .keepwarm_tail_us = 2000,
		.last_done_ns = now - US(500), .last_tickle_ns = now - US(30),
		.now_ns = now, .lock_free = true, .device_ready = true,
	};
	ane_t6021_keepwarm_plan(&in, &out);
	check("T5d not due: sleeps the remaining interval",
	      out.sleep_us == 70);

	/* T6 device not ready: never ring the doorbell. */
	in = (struct ane_t6021_keepwarm_in){
		.keepwarm_us = 50, .keepwarm_tail_us = 2000,
		.last_done_ns = now - US(500), .last_tickle_ns = now - US(100),
		.now_ns = now, .lock_free = true, .device_ready = false,
	};
	ane_t6021_keepwarm_plan(&in, &out);
	check("T6a device not ready: no tickle", !out.tickle);
	check("T6b device not ready: retries after one interval",
	      out.sleep_us == 50);

	if (failures) {
		printf("%d failure(s)\n", failures);
		return 1;
	}
	printf("keepwarm_test: all checks pass\n");
	return 0;
}
