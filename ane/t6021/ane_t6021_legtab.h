/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * ane_t6021_legtab.h: the rules of the driver's table of buffers the firmware asked for on its SHAREDMALLOC ring, factored out so the host unit test
 * (tools/test_t6021_legtab.c) compiles the rules the driver runs.
 *
 * Slots. The table has `cap` slots and a byte budget. A buffer is handed out for a MALLOC request and comes back with the firmware's FREE of its IOVA. Before, a
 * freed buffer stayed in the table until teardown, so every block the firmware freed and asked for again used a new slot and new bytes, and a few sessions of the
 * unload filled the 512 MiB budget (the next MALLOC then failed and the device was quarantined). Now a FREE releases the slot and its bytes, and a MALLOC takes
 * the first free slot below the high-water mark before it grows the table. A slot is free when its cpu pointer is NULL.
 *
 * Owners. A buffer is stamped with the program whose command was running when the firmware asked for it (program id + 1, or ANE_LEGTAB_NO_OWNER). A LOAD does
 * not know its program id until the firmware answers, so its buffers carry ANE_LEGTAB_PENDING until ane_legtab_restamp() gives them to the new program.
 * Unloading a program must not release its section buffers while the firmware still has a FREE to ask for, so it waits until every buffer stamped for the
 * program has been given back (ane_legtab_wait_owner), and not for a fixed time.
 */
#ifndef __ANE_T6021_LEGTAB_H__
#define __ANE_T6021_LEGTAB_H__

#include <linux/errno.h>
#include <linux/types.h>

#define ANE_LEGTAB_NO_OWNER	0u
#define ANE_LEGTAB_PENDING	0xffffffffu

struct ane_legacy_buffer {
	void *cpu;
	dma_addr_t dma;
	size_t size;
	u32 owner;	/* ANE_LEGTAB_NO_OWNER, ANE_LEGTAB_PENDING, or program id + 1 */
};

/* The slot a new buffer takes: the first free one below `used`, else `used` itself while the table can grow, else -1. */
static inline int ane_legtab_pick(const struct ane_legacy_buffer *t, u32 used, u32 cap)
{
	u32 i;

	for (i = 0; i < used; i++)
		if (!t[i].cpu)
			return (int)i;
	return used < cap ? (int)used : -1;
}

/* The slot that holds the buffer whose DMA address is `dma`, or -1. `keep` (may be NULL) is a buffer the driver itself still uses and that a FREE must
 * never match. */
static inline int ane_legtab_find(const struct ane_legacy_buffer *t, u32 used, u64 dma, const struct ane_legacy_buffer *keep)
{
	u32 i;

	for (i = 0; i < used; i++)
		if (t[i].cpu && t[i].dma == dma && &t[i] != keep)
			return (int)i;
	return -1;
}

/* How many buffers stamped `owner` the firmware still holds. */
static inline u32 ane_legtab_owned(const struct ane_legacy_buffer *t, u32 used, u32 owner)
{
	u32 i, n = 0;

	for (i = 0; i < used; i++)
		if (t[i].cpu && t[i].owner == owner)
			n++;
	return n;
}

/* Give every buffer stamped `from` to `to`. */
static inline void ane_legtab_restamp(struct ane_legacy_buffer *t, u32 used, u32 from, u32 to)
{
	u32 i;

	for (i = 0; i < used; i++)
		if (t[i].owner == from)
			t[i].owner = to;
}

struct ane_legtab_wait_ops {
	int (*serve)(void *ctx);	/* answer one posted ring entry: 1 = answered one, 0 = none posted, < 0 = the ring is not what we think */
	u64 (*now_us)(void *ctx);
	void (*pause)(void *ctx);	/* give the CPU away for a short while between two looks at an empty ring */
};

/* Wait until every buffer stamped `owner` has been given back, answering the ring while it waits. The ring is looked at once before anything else is
 * tested, so a bound of 0 still answers what is posted. Returns 0 when none is stamped or as soon as the last one is back, -ETIMEDOUT when some are
 * still out bound_us after the start, or the negative result of serve(). `used` is the table's high-water mark, which grows while the wait runs. */
static inline int ane_legtab_wait_owner(const struct ane_legtab_wait_ops *ops, void *ctx, const struct ane_legacy_buffer *t, const u32 *used,
					u32 owner, u64 bound_us)
{
	u64 start = ops->now_us(ctx);

	for (;;) {
		int served = ops->serve(ctx);

		if (served < 0)
			return served;
		if (!ane_legtab_owned(t, *used, owner))
			return 0;
		if (ops->now_us(ctx) - start >= bound_us)
			return -ETIMEDOUT;
		if (!served)
			ops->pause(ctx);
	}
}

#endif /* __ANE_T6021_LEGTAB_H__ */
