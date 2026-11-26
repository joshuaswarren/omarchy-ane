/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * ane_h18_progtab.h: kernel-side binding lifetime for N resident H18 programs.
 *
 * Host table limit (ANE_H18_MAX_RESIDENT = 16) is a host-driver cap, not a
 * hardware limit. Each resident slot owns its parsed ANEP layout, DMA surfaces,
 * fixed-IOVA alias reservations, firmware program/process IDs, and a monotonic
 * generation-tagged u64 handle ((u64)gen << 32 | slot_idx, gen >= 1).
 *
 * State machine per slot:
 *   FREE -> LIVE: LOAD_PROGRAM and CREATE_PROCESS both succeeded; surfaces and
 *                 fixed aliases remain mapped until explicit free or close.
 *   LIVE -> FREE: TERMINATE_PROCESS and DESTROY_PROGRAM both succeeded AND
 *                 every fixed alias unmapped cleanly; slot and ranges return
 *                 to the pool (next retain bumps gen so stale handles fail
 *                 with -ENOENT).
 *   LIVE/loading -> HELD: firmware unload failed/timed out OR a fixed alias
 *                 unmap came back short. Surfaces and fixed-IOVA reservations
 *                 are retained forever, the slot is never reused, and the
 *                 module stays pinned.
 *
 * Fixed-IOVA overlap is refused (-EBUSY) against every LIVE and HELD slot
 * before any DMA allocation, IOMMU map, or LOAD_PROGRAM command.
 */
#ifndef __ANE_H18_PROGTAB_H__
#define __ANE_H18_PROGTAB_H__

#include <linux/errno.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/types.h>

#include "ane_h18.h"

#define ANE_H18_MAX_RESIDENT		16
#define ANE_H18_SLOT_MAX_RANGES		(2 + ANE_H18_PROG_MAX_IO)
#define ANE_H18_FIXED_IOVA_LIMIT	0xe0000000ULL
#define ANE_H18_RES_NOID		0xffffffffu

enum ane_h18_slot_state {
	ANE_H18_SLOT_FREE = 0,
	ANE_H18_SLOT_LIVE,
	ANE_H18_SLOT_HELD,
};

struct ane_h18_iova_range {
	u64 start;
	u64 size;
};

struct sg_table;

struct ane_h18_hs_buf {
	void *va;
	struct sg_table *sgt;	/* non-NULL when allocated via dma_alloc_noncontiguous + dma_vmap_noncontiguous */
	dma_addr_t iova;	/* the address the firmware is given: the fixed alias when `fixed`, else the DMA API address */
	dma_addr_t dma;		/* the address DMA API returned: what dma_free_* needs */
	size_t size;
	bool fixed;		/* an alias mapping at `iova` was added with iommu_map and is removed with iommu_unmap before the free */
	bool leaked;		/* a short alias unmap (free or partial-map rollback) retained the pages and range; never dma_free_* */
};

struct ane_h18_pdesc {
	unsigned int nsec, nio;
	struct { u32 id; u64 size, off; } sec[ANE_H18_PROG_MAX_SECS];
	struct { u32 id, dir; u64 size, doff, dsize; } io[ANE_H18_PROG_MAX_IO];
};

struct ane_h18_res_opts {
	u64 iova_bufs;
	u64 iova_text;
	u64 iova_kern;
	u32 text_const;
	bool kern_slot;
};

struct ane_h18_res_slot {
	enum ane_h18_slot_state state;
	u32 gen;
	u64 handle;
	u32 prog_id;
	u32 proc_id;
	struct ane_h18_pdesc pd;
	struct ane_h18_res_opts opts;
	struct ane_h18_hs_buf pg_sec[ANE_H18_PROG_MAX_SECS];
	struct ane_h18_hs_buf pg_kern;
	struct ane_h18_hs_buf pg_io[ANE_H18_PROG_MAX_IO];
	struct ane_h18_iova_range ranges[ANE_H18_SLOT_MAX_RANGES];
	unsigned int nranges;
};

struct ane_h18_res_tab {
	struct ane_h18_res_slot slots[ANE_H18_MAX_RESIDENT];
	u32 next_gen;
};

static inline bool ane_h18_range_valid(u64 start, u64 size)
{
	return size > 0 && !(start & (SZ_16K - 1)) && start < ANE_H18_FIXED_IOVA_LIMIT &&
	       size <= ANE_H18_FIXED_IOVA_LIMIT - start;
}

static inline bool ane_h18_ranges_overlap(const struct ane_h18_iova_range *a,
					  const struct ane_h18_iova_range *b)
{
	if (!a->size || !b->size)
		return false;
	if (a->start <= b->start)
		return b->start - a->start < a->size;
	return a->start - b->start < b->size;
}

/* Validate program options and collect every fixed-IOVA interval the candidate
 * would map. Refuses bad alignment/bounds, invalid const/kern layouts, and
 * self-overlaps with -EINVAL before any DMA allocation or IOMMU call. */
static inline int ane_h18_res_plan_ranges(const struct ane_h18_pdesc *p,
					  const struct ane_h18_res_opts *opts,
					  struct ane_h18_iova_range *out,
					  unsigned int *nout)
{
	const u64 cbytes = 0x4000;
	unsigned int i, j, n = 0;
	unsigned int s2 = p->nsec, s3 = p->nsec;

	if (opts->kern_slot && !opts->iova_kern)
		return -EINVAL;
	for (i = 0; i < p->nsec; i++) {
		if (p->sec[i].id == 2)
			s2 = i;
		if (p->sec[i].id == 3)
			s3 = i;
	}
	if (opts->text_const || opts->iova_kern) {
		if (s2 == p->nsec || p->sec[s2].size < cbytes)
			return -EINVAL;
		if (opts->text_const) {
			if (!opts->iova_text || s3 == p->nsec || (opts->text_const & 7) ||
			    opts->text_const + cbytes > 0x8000 || p->sec[s3].size > opts->text_const)
				return -EINVAL;
		}
		if (opts->iova_kern && p->sec[s2].size <= cbytes)
			return -EINVAL;
	}
	if (opts->iova_text) {
		u64 sz;

		if (s3 == p->nsec)
			return -EINVAL;
		sz = ALIGN(p->sec[s3].size, 0x8000);
		if (!ane_h18_range_valid(opts->iova_text, sz))
			return -EINVAL;
		out[n++] = (struct ane_h18_iova_range){ opts->iova_text, sz };
	}
	if (opts->iova_kern) {
		u64 sz = ALIGN(p->sec[s2].size - cbytes, SZ_16K);

		if (!ane_h18_range_valid(opts->iova_kern, sz))
			return -EINVAL;
		out[n++] = (struct ane_h18_iova_range){ opts->iova_kern, sz };
	}
	if (opts->iova_bufs) {
		u64 at = opts->iova_bufs;

		for (i = 0; i < p->nio; i++) {
			u64 sz = ALIGN(p->io[i].size, SZ_16K);

			if (!ane_h18_range_valid(at, sz))
				return -EINVAL;
			out[n++] = (struct ane_h18_iova_range){ at, sz };
			at += sz;
		}
	}
	for (i = 0; i < n; i++)
		for (j = i + 1; j < n; j++)
			if (ane_h18_ranges_overlap(&out[i], &out[j]))
				return -EINVAL;
	*nout = n;
	return 0;
}

/* Check candidate fixed-IOVA intervals against all LIVE and HELD slots.
 * Returns -EBUSY on any overlap, 0 when disjoint. */
static inline int ane_h18_res_check_overlap(const struct ane_h18_res_tab *t,
					    const struct ane_h18_iova_range *cand,
					    unsigned int ncand)
{
	unsigned int s, i, j;

	for (s = 0; s < ANE_H18_MAX_RESIDENT; s++) {
		const struct ane_h18_res_slot *slot = &t->slots[s];

		if (slot->state == ANE_H18_SLOT_FREE)
			continue;
		for (i = 0; i < ncand; i++)
			for (j = 0; j < slot->nranges; j++)
				if (ane_h18_ranges_overlap(&cand[i], &slot->ranges[j]))
					return -EBUSY;
	}
	return 0;
}

static inline struct ane_h18_res_slot *ane_h18_res_alloc_slot(struct ane_h18_res_tab *t,
							      unsigned int *idx_out)
{
	unsigned int i;

	for (i = 0; i < ANE_H18_MAX_RESIDENT; i++) {
		if (t->slots[i].state == ANE_H18_SLOT_FREE) {
			*idx_out = i;
			return &t->slots[i];
		}
	}
	return NULL;
}

static inline u64 ane_h18_res_activate(struct ane_h18_res_tab *t, unsigned int idx,
				       u32 prog_id, u32 proc_id,
				       const struct ane_h18_iova_range *ranges,
				       unsigned int nranges)
{
	struct ane_h18_res_slot *slot = &t->slots[idx];
	u32 gen = ++t->next_gen;

	if (!gen)
		gen = ++t->next_gen;
	slot->gen = gen;
	slot->handle = ((u64)gen << 32) | (u64)idx;
	slot->prog_id = prog_id;
	slot->proc_id = proc_id;
	slot->nranges = nranges;
	if (nranges)
		memcpy(slot->ranges, ranges, nranges * sizeof(*ranges));
	slot->state = ANE_H18_SLOT_LIVE;
	return slot->handle;
}

/* Resolve a generation-tagged handle to a LIVE slot.
 * Unknown, freed, or stale-generation handles -> -ENOENT.
 * HELD slots (failed unload / short unmap) -> -ESHUTDOWN. */
static inline struct ane_h18_res_slot *ane_h18_res_lookup(struct ane_h18_res_tab *t,
							  u64 handle, int *err_out)
{
	u32 idx = (u32)(handle & 0xffffffffULL);
	u32 gen = (u32)(handle >> 32);
	struct ane_h18_res_slot *slot;

	if (!handle || !gen || idx >= ANE_H18_MAX_RESIDENT) {
		*err_out = -ENOENT;
		return NULL;
	}
	slot = &t->slots[idx];
	if (slot->state == ANE_H18_SLOT_FREE || slot->handle != handle || slot->gen != gen) {
		*err_out = -ENOENT;
		return NULL;
	}
	if (slot->state == ANE_H18_SLOT_HELD) {
		*err_out = -ESHUTDOWN;
		return NULL;
	}
	*err_out = 0;
	return slot;
}

static inline void ane_h18_res_mark_held(struct ane_h18_res_slot *slot,
					 const struct ane_h18_iova_range *ranges,
					 unsigned int nranges)
{
	slot->state = ANE_H18_SLOT_HELD;
	if (ranges) {
		slot->nranges = nranges;
		if (nranges)
			memcpy(slot->ranges, ranges, nranges * sizeof(*ranges));
	}
}

static inline void ane_h18_res_mark_free(struct ane_h18_res_slot *slot)
{
	memset(slot, 0, sizeof(*slot));
}

static inline unsigned int ane_h18_res_count_state(const struct ane_h18_res_tab *t,
						   enum ane_h18_slot_state state)
{
	unsigned int i, n = 0;

	for (i = 0; i < ANE_H18_MAX_RESIDENT; i++)
		if (t->slots[i].state == state)
			n++;
	return n;
}

/* True if any LIVE or HELD slot already owns this firmware prog_id. */
static inline bool ane_h18_res_has_prog_id(const struct ane_h18_res_tab *t, u32 prog_id)
{
	unsigned int i;

	for (i = 0; i < ANE_H18_MAX_RESIDENT; i++) {
		const struct ane_h18_res_slot *slot = &t->slots[i];

		if (slot->state != ANE_H18_SLOT_FREE && slot->prog_id != ANE_H18_RES_NOID &&
		    slot->prog_id == prog_id)
			return true;
	}
	return false;
}

#endif /* __ANE_H18_PROGTAB_H__ */
