// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

/*
// test_t6021_pool.c — host unit test for the parked-BO pool policy in
// ane/t6021/ane_t6021_pool.h (the same header the driver compiles, over
// the shims in ane_pool_shim/linux). Cases:
//   - admission: budget off, below the size floor, at/above the floor
//   - same-size reuse: a parked entry comes back for the same
//     page-aligned size and its budget charge clears (the same-IOVA
//     guarantee: the driver hands the same dma range back)
//   - LRU eviction: a park that would exceed the budget really frees
//     the OLDEST budgeted entries until it fits; uncharged io parks are
//     never evicted
//   - quarantine: take refuses while quarantined, entry survives
//   - unreachable budget: park fails with -ENOSPC, nothing parked
*/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <linux/list.h>		/* the shim: list_head, container_of */

#include "../ane/t6021/ane_t6021_pool.h"

#define POOL_TEST_MAX_MIB	400
#define POOL_TEST_MIN_KIB	2048

struct fake_bo {
	struct ane_t6021_pool_ent ent;
	int id;
	int freed;
};

static struct fake_bo *freed_log[16];
static unsigned n_freed;

static void fake_release(struct ane_t6021_pool_ent *ent)
{
	struct fake_bo *b = container_of(ent, struct fake_bo, ent);

	b->freed++;
	freed_log[n_freed++] = b;
}

static struct fake_bo fake_bo_new(int id, size_t size)
{
	struct fake_bo b = { .id = id };

	memset(&b, 0, sizeof(b));
	b.id = id;
	b.ent.size = size;
	return b;
}

static int failures;
static int before;

static void check(int cond, const char *what)
{
	if (!cond) {
		printf("FAIL %s\n", what);
		failures++;
	}
}

int main(void)
{
	struct ane_t6021_pool p;
	struct fake_bo a, b, c, d, u, e;
	struct ane_t6021_pool_ent *ent;

	before = failures;

	/* Admission policy. */
	ane_t6021_pool_init(&p, 0, POOL_TEST_MIN_KIB);
	check(!ane_t6021_pool_admits(&p, 256u << 20),
	      "admission: budget off must refuse");
	ane_t6021_pool_init(&p, POOL_TEST_MAX_MIB, POOL_TEST_MIN_KIB);
	check(!ane_t6021_pool_admits(&p, 1u << 20),
	      "admission: below the 2 MiB floor must refuse");
	check(ane_t6021_pool_admits(&p, 2u << 20),
	      "admission: at the floor must pass");
	check(ane_t6021_pool_admits(&p, 213u << 20),
	      "admission: a 213 MiB section must pass");

	/* Same-size reuse: the charge clears, the entry comes back. */
	a = fake_bo_new(1, 16u << 20);
	check(ane_t6021_pool_park(&p, &a.ent, fake_release) == 0,
	      "park: 16 MiB parks");
	check(ane_t6021_pool_sec_bytes(&p) == (s64)PAGE_ALIGN(16u << 20),
	      "park: budget charged once");
	ent = ane_t6021_pool_take(&p, 16u << 20, false);
	check(ent == &a.ent, "reuse: same-size take returns the SAME entry");
	check(ane_t6021_pool_sec_bytes(&p) == 0,
	      "reuse: budget charge cleared on take");
	check(!ane_t6021_pool_take(&p, 16u << 20, false),
	      "reuse: pool is empty after the take");

	/* LRU eviction: oldest budgeted entries free first; an uncharged
	 * io park is never evicted. */
	ane_t6021_pool_init(&p, POOL_TEST_MAX_MIB, POOL_TEST_MIN_KIB);
	a = fake_bo_new(1, 100u << 20);
	b = fake_bo_new(2, 200u << 20);
	c = fake_bo_new(3, 300u << 20);
	u = fake_bo_new(9, 8u << 20);
	ane_t6021_pool_park_uncharged(&p, &u.ent);
	check(ane_t6021_pool_park(&p, &a.ent, fake_release) == 0,
	      "evict: 100 MiB parks");
	check(ane_t6021_pool_park(&p, &b.ent, fake_release) == 0,
	      "evict: 200 MiB parks");
	check(ane_t6021_pool_park(&p, &c.ent, fake_release) == 0,
	      "evict: 300 MiB parks after evicting to fit");
	check(n_freed == 2 && freed_log[0] == &a && freed_log[1] == &b,
	      "evict: the OLDEST budgeted entries freed, in order");
	check(u.freed == 0, "evict: the uncharged io park survives");
	check(ane_t6021_pool_sec_bytes(&p) == (s64)PAGE_ALIGN(300u << 20),
	      "evict: budget holds only the parked survivor");
	check(ane_t6021_pool_take(&p, 100u << 20, false) == NULL,
	      "evict: the evicted entry is gone");
	ent = ane_t6021_pool_take(&p, 8u << 20, false);
	check(ent == &u.ent, "uncharged park still handed out");

	/* Quarantine: nothing leaves the pool while quarantined. */
	d = fake_bo_new(4, 64u << 20);
	ane_t6021_pool_init(&p, POOL_TEST_MAX_MIB, POOL_TEST_MIN_KIB);
	check(ane_t6021_pool_park(&p, &d.ent, fake_release) == 0,
	      "quarantine: 64 MiB parks");
	check(ane_t6021_pool_take(&p, 64u << 20, true) == NULL,
	      "quarantine: take refuses");
	ent = ane_t6021_pool_take(&p, 64u << 20, false);
	check(ent == &d.ent, "quarantine: entry intact after the lift");

	/* Unreachable budget: park refuses, nothing is parked or freed. */
	n_freed = 0;
	ane_t6021_pool_init(&p, 1, POOL_TEST_MIN_KIB);
	e = fake_bo_new(5, 4u << 20);
	check(ane_t6021_pool_park(&p, &e.ent, fake_release) == -ENOSPC,
	      "unreachable: park fails with -ENOSPC");
	check(n_freed == 0 && ane_t6021_pool_sec_bytes(&p) == 0,
	      "unreachable: nothing freed, nothing parked");

	printf(failures == before ? "POOL-CHECK PASS\n" :
				    "POOL-CHECK FAIL\n");
	return failures != before;
}
