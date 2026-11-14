// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

/*
// test_t6021_legtab.c: host unit test for ane/t6021/ane_t6021_legtab.h, the rules of the table of firmware MALLOC buffers. The M2 soak of the program unload
// failed on the fourth session because freed buffers kept their slot and bytes. Slot cases: a new buffer takes the first free slot below the high-water mark,
// the table grows only when no slot is free, a full table returns -1, a FREE finds its buffer by DMA address and never the buffer marked `keep`, an empty or
// freed slot never matches, and a free-then-malloc loop of many rounds never grows the table.
// Wait cases (ane_legtab_wait_owner, run against a fake firmware on a virtual clock): a FREE already posted, a FREE at 39 ms, a FREE at 300 ms that the old
// 100 ms TERMINATE window missed, no FREE at all (gives up at the bound), a FREE 1 us before and 1 us after the bound, a bound of 0, two buffers out for one
// program, a second program's buffers that must not hold the wait up, PENDING buffers that join a program on restamp, a buffer asked for during the wait,
// and an error from the ring.
*/

#include <stdio.h>
#include <string.h>

#include "../ane/t6021/ane_t6021_legtab.h"

static int failures;

static void check(int cond, const char *what)
{
	if (!cond) {
		fprintf(stderr, "FAIL %s\n", what);
		failures++;
	}
}

/* A fake firmware on a virtual clock. Its ring holds events that appear at a time: a FREE of a table slot, or a MALLOC that stamps a new buffer. serve() answers
 * the next event that has appeared. The clock moves only in pause(), 50 us each, like the driver's sleep between two looks at an empty ring. */
enum ev_kind { EV_FREE, EV_MALLOC };

struct fw_event {
	u64 at;
	enum ev_kind kind;
	int slot;
	u32 owner;	/* EV_MALLOC: the stamp of the new buffer */
	bool done;
};

struct fake_fw {
	struct ane_legacy_buffer t[8];
	u32 used;
	char mem[8];
	u64 now;
	struct fw_event ev[8];
	int nev;
	int fail_on_call;	/* serve() returns -EOPNOTSUPP on this call, 0 = never */
	int calls;
	int pauses;
};

static void fw_buffer(struct fake_fw *f, int k, u32 owner)
{
	f->t[k] = (struct ane_legacy_buffer){ &f->mem[k], 0x10000 + 0x1000 * k, 0x1000, owner };
	if ((u32)k >= f->used)
		f->used = k + 1;
}

static void fw_at(struct fake_fw *f, u64 at, enum ev_kind kind, int slot, u32 owner)
{
	f->ev[f->nev++] = (struct fw_event){ at, kind, slot, owner, false };
}

static int fw_serve(void *ctx)
{
	struct fake_fw *f = ctx;
	int i;

	if (++f->calls == f->fail_on_call)
		return -EOPNOTSUPP;
	for (i = 0; i < f->nev; i++) {
		struct fw_event *e = &f->ev[i];

		if (e->done || e->at > f->now)
			continue;
		e->done = true;
		if (e->kind == EV_FREE)
			memset(&f->t[e->slot], 0, sizeof(f->t[0]));
		else
			fw_buffer(f, e->slot, e->owner);
		return 1;
	}
	return 0;
}

static u64 fw_now(void *ctx)
{
	return ((struct fake_fw *)ctx)->now;
}

static void fw_pause(void *ctx)
{
	struct fake_fw *f = ctx;

	f->now += 50;
	f->pauses++;
}

static const struct ane_legtab_wait_ops fw_ops = { fw_serve, fw_now, fw_pause };

static int fw_wait(struct fake_fw *f, u32 owner, u64 bound_us)
{
	return ane_legtab_wait_owner(&fw_ops, f, f->t, &f->used, owner, bound_us);
}

static void test_wait(void)
{
	struct fake_fw f;

	memset(&f, 0, sizeof(f));
	fw_buffer(&f, 0, ANE_LEGTAB_NO_OWNER);
	check(fw_wait(&f, 4, 500000) == 0 && f.now == 0 && f.pauses == 0, "a buffer nobody owns holds no wait up");

	memset(&f, 0, sizeof(f));
	fw_buffer(&f, 0, 4);
	fw_at(&f, 0, EV_FREE, 0, 0);
	check(fw_wait(&f, 4, 500000) == 0 && f.pauses == 0 && !f.t[0].cpu, "a FREE already posted ends the wait with no pause");

	memset(&f, 0, sizeof(f));
	fw_buffer(&f, 0, 4);
	fw_at(&f, 39000, EV_FREE, 0, 0);
	check(fw_wait(&f, 4, 500000) == 0 && f.now >= 39000 && f.now < 39050 && !f.t[0].cpu, "a FREE at 39 ms ends the wait within one pause of it");

	memset(&f, 0, sizeof(f));
	fw_buffer(&f, 0, 4);
	fw_at(&f, 300000, EV_FREE, 0, 0);
	check(fw_wait(&f, 4, 500000) == 0 && !f.t[0].cpu && f.now >= 300000 && f.now < 300050, "a late FREE at 300 ms is answered inside a 500 ms bound");

	memset(&f, 0, sizeof(f));
	fw_buffer(&f, 0, 4);
	check(fw_wait(&f, 4, 20000) == -ETIMEDOUT && f.now >= 20000 && f.now < 20050 && f.t[0].cpu, "no FREE: -ETIMEDOUT at the bound, buffer still out");

	memset(&f, 0, sizeof(f));
	fw_buffer(&f, 0, 4);
	fw_at(&f, 19999, EV_FREE, 0, 0);
	check(fw_wait(&f, 4, 20000) == 0, "a FREE 1 us before the bound ends the wait with 0");
	memset(&f, 0, sizeof(f));
	fw_buffer(&f, 0, 4);
	fw_at(&f, 20001, EV_FREE, 0, 0);
	check(fw_wait(&f, 4, 20000) == -ETIMEDOUT, "a FREE 1 us after the bound is not waited for");

	memset(&f, 0, sizeof(f));
	fw_buffer(&f, 0, 4);
	fw_at(&f, 0, EV_FREE, 0, 0);
	check(fw_wait(&f, 4, 0) == 0, "bound 0: a FREE already posted is answered");
	memset(&f, 0, sizeof(f));
	fw_buffer(&f, 0, 4);
	check(fw_wait(&f, 4, 0) == -ETIMEDOUT && f.pauses == 0, "bound 0: nothing posted gives up with no pause");

	memset(&f, 0, sizeof(f));
	fw_buffer(&f, 0, 4);
	fw_buffer(&f, 1, 4);
	fw_at(&f, 2000, EV_FREE, 0, 0);
	fw_at(&f, 9000, EV_FREE, 1, 0);
	check(fw_wait(&f, 4, 500000) == 0 && f.now == 9000, "two buffers out: the wait ends at the last FREE, not the first");

	memset(&f, 0, sizeof(f));
	fw_buffer(&f, 0, 3);
	fw_buffer(&f, 1, 4);
	fw_at(&f, 5000, EV_FREE, 1, 0);
	fw_at(&f, 12000, EV_FREE, 0, 0);
	check(fw_wait(&f, 4, 500000) == 0 && f.now == 5000 && f.t[0].cpu && !f.t[1].cpu, "a wait ends at its own program's FREE, not at the other's");
	check(fw_wait(&f, 3, 500000) == 0 && f.now == 12000 && !f.t[0].cpu, "the other program's wait finds its FREE later");

	memset(&f, 0, sizeof(f));
	fw_buffer(&f, 0, ANE_LEGTAB_PENDING);
	fw_at(&f, 7000, EV_FREE, 0, 0);
	check(fw_wait(&f, 5, 500000) == 0 && f.now == 0 && f.t[0].cpu, "a PENDING buffer is nobody's: a wait for owner 5 returns at once");
	ane_legtab_restamp(f.t, f.used, ANE_LEGTAB_PENDING, 5);
	check(fw_wait(&f, 5, 500000) == 0 && f.now == 7000 && !f.t[0].cpu, "after restamp the wait for owner 5 ends at the FREE");

	memset(&f, 0, sizeof(f));
	fw_buffer(&f, 0, 4);
	fw_at(&f, 2000, EV_MALLOC, 1, 4);
	fw_at(&f, 3000, EV_FREE, 0, 0);
	fw_at(&f, 6000, EV_FREE, 1, 0);
	check(fw_wait(&f, 4, 500000) == 0 && f.now == 6000 && f.used == 2 && !f.t[1].cpu, "a buffer asked for during the wait is waited for too");

	memset(&f, 0, sizeof(f));
	fw_buffer(&f, 0, 4);
	f.fail_on_call = 3;
	check(fw_wait(&f, 4, 500000) == -EOPNOTSUPP && f.t[0].cpu, "an error from the ring is returned");
}

/* ane_legtab_free_log_info: one answered FREE logs at info level only when the answer says something new. The 14-inch M2 Max soak of 2026-11-13 measured about
 * 1744 info lines in 15 minutes (about 1.9 per second) from the per-answer line alone, which drowned fault lines elsewhere in the kernel log. Clock cases:
 * 1000 answers of the same buffer in half a second log one info line, an answer 10 s after the last line logs again, a new iova logs inside the window, a
 * grown table and an unmatched FREE log whatever the clock says, and after every info line the same answer goes quiet again. */
static void test_free_log(void)
{
	struct ane_legtab_free_log s;
	u64 now = 0;
	int i, infos = 0;

	memset(&s, 0, sizeof(s));
	for (i = 0; i < 1000; i++) {
		now += 500;	/* the measured device pace, about two answers a second */
		infos += ane_legtab_free_log_info(&s, now, 0xeb800000, 10, 0);
	}
	check(infos == 1, "1000 answers of the same buffer in 0.5 s log exactly one info line");

	check(ane_legtab_free_log_info(&s, now + ANE_LEGTAB_FREE_INFO_PERIOD_US, 0xeb800000, 10, 0) == 1,
	      "an answer 10 s after the last info line logs again");
	check(ane_legtab_free_log_info(&s, now + ANE_LEGTAB_FREE_INFO_PERIOD_US + 500, 0xeb800000, 10, 0) == 0,
	      "the same answer right after it stays at debug");
	check(ane_legtab_free_log_info(&s, now + ANE_LEGTAB_FREE_INFO_PERIOD_US + 1000, 0x1230000, 10, 0) == 1,
	      "a changed iova logs at info inside the window");
	check(ane_legtab_free_log_info(&s, now + ANE_LEGTAB_FREE_INFO_PERIOD_US + 1500, 0x1230000, 11, 0) == 1,
	      "a grown slots high water logs at info inside the window");
	check(ane_legtab_free_log_info(&s, now + ANE_LEGTAB_FREE_INFO_PERIOD_US + 2000, 0x1230000, 11, -1) == 1,
	      "an unmatched FREE logs at info inside the window");
	check(ane_legtab_free_log_info(&s, now + ANE_LEGTAB_FREE_INFO_PERIOD_US + 2500, 0x1230000, 11, -1) == 1,
	      "an unmatched FREE is never suppressed");
	check(s.high_water == 11 && s.last_iova == 0x1230000, "the state tracks the last answer");
}

int main(void)
{
	struct ane_legacy_buffer t[8];
	char mem[8];
	u32 used = 0;
	int i, k, round;

	memset(t, 0, sizeof(t));
	check(ane_legtab_pick(t, 0, 8) == 0, "an empty table hands out slot 0");
	for (i = 0; i < 3; i++) {
		k = ane_legtab_pick(t, used, 8);
		check(k == i, "the table grows one slot at a time");
		t[k] = (struct ane_legacy_buffer){ &mem[i], 0x1000 + 0x1000 * i, 0x1000, ANE_LEGTAB_NO_OWNER };
		if ((u32)k == used)
			used++;
	}
	check(ane_legtab_find(t, used, 0x2000, NULL) == 1, "a FREE finds the buffer by DMA address");
	check(ane_legtab_find(t, used, 0x2000, &t[1]) == -1, "the kept buffer is never matched");
	check(ane_legtab_find(t, used, 0x9000, NULL) == -1, "an unknown address matches nothing");
	/* a slot with a stale DMA address but no cpu pointer is free and never matches */
	t[2].cpu = NULL;
	check(ane_legtab_find(t, used, 0x3000, NULL) == -1, "a slot without a cpu pointer never matches, whatever its stale address");
	t[2].cpu = &mem[2];
	/* free slot 1: it is the next one handed out, the table does not grow */
	memset(&t[1], 0, sizeof(t[1]));
	check(ane_legtab_find(t, used, 0x2000, NULL) == -1, "a freed slot no longer matches");
	check(ane_legtab_pick(t, used, 8) == 1, "the first free slot below the high-water mark is reused");
	/* fill up to the cap, then no slot */
	for (; used < 8;) {
		k = ane_legtab_pick(t, used, 8);
		t[k] = (struct ane_legacy_buffer){ &mem[k], 0x1000 * (k + 1), 0x1000, ANE_LEGTAB_NO_OWNER };
		if ((u32)k == used)
			used++;
	}
	check(ane_legtab_pick(t, used, 8) == -1, "a full table returns -1");
	/* many rounds of free and malloc of the same slot count never grow the table */
	for (round = 0; round < 1000; round++) {
		memset(&t[round % 8], 0, sizeof(t[0]));
		k = ane_legtab_pick(t, used, 8);
		check(k == round % 8, "free then malloc reuses the freed slot");
		t[k] = (struct ane_legacy_buffer){ &mem[k], 0x100000 + round, 0x1000, ANE_LEGTAB_NO_OWNER };
	}
	check(used == 8, "the high-water mark stays at 8 over 1000 rounds");
	test_wait();
	test_free_log();
	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("test_t6021_legtab: all checks passed\n");
	return 0;
}
