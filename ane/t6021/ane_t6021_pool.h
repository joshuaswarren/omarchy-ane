/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * ane_t6021_pool.h — the parked-BO pool policy of the T6021 driver,
 * factored out so the host unit test (tools/test_t6021_pool.c) compiles
 * the very code the driver runs (the ane_stats_shim pattern: the host
 * test maps linux/list.h, linux/spinlock.h, linux/atomic.h,
 * linux/types.h and PAGE_ALIGN onto small shims).
 *
 * The pool holds buffer objects whose last reference is gone but whose
 * memory and IOVA stay reserved: an io BO the firmware may write again,
 * and a freed duplicate section BO that a later same-size BO_INIT
 * reuses. Reuse is the point: handing the SAME dma range back for a
 * repeated load keeps the program from churning the dma32 window into
 * fragments no 224 MiB section fits any more (measured on the M2,
 * 2026-10-09: after configure passes, a 222,980,416-byte BO_INIT failed
 * in every process at bo_total 2.76 GB).
 *
 * Section parking is budgeted (max_mb) and the oldest budgeted entry is
 * evicted by real free when a park would exceed the budget, so the held
 * total stays inside the window: nominal 4 GiB (the 32-bit DMA mask),
 * observed usable >= 2.98 GB, floor 2.754 GB, budget default 512 MiB.
 */

#include <linux/types.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/mm.h>
#include <linux/sizes.h>

struct ane_t6021_pool_ent;

typedef void (*ane_t6021_pool_release_fn)(struct ane_t6021_pool_ent *ent);

struct ane_t6021_pool_ent {
	struct list_head node;
	size_t size;
	bool pooled_sec;	/* charged against the section budget */
};

struct ane_t6021_pool {
	struct list_head list;	/* oldest first */
	spinlock_t lock;
	atomic64_t sec_bytes;
	unsigned int max_mb;	/* section-park budget, MiB; 0 = off */
	unsigned int min_kb;	/* smallest parkable section, KiB */
};

static inline void ane_t6021_pool_init(struct ane_t6021_pool *p,
				       unsigned int max_mb,
				       unsigned int min_kb)
{
	INIT_LIST_HEAD(&p->list);
	spin_lock_init(&p->lock);
	atomic64_set(&p->sec_bytes, 0);
	p->max_mb = max_mb;
	p->min_kb = min_kb;
}

/* Admission policy: SIZE (raw bytes) is parkable now? */
static inline bool ane_t6021_pool_admits(const struct ane_t6021_pool *p,
					 size_t size)
{
	return p->max_mb != 0 &&
	       PAGE_ALIGN(size) >= (size_t)p->min_kb * SZ_1K;
}

/* Unlink and uncharge the OLDEST budgeted entry, or NULL. The caller
 * does the real free (dma_free_coherent may not run under a spinlock). */
static inline struct ane_t6021_pool_ent *
ane_t6021_pool_evict_oldest(struct ane_t6021_pool *p)
{
	struct ane_t6021_pool_ent *e;

	spin_lock(&p->lock);
	list_for_each_entry(e, &p->list, node) {
		if (e->pooled_sec) {
			atomic64_sub(PAGE_ALIGN(e->size), &p->sec_bytes);
			list_del(&e->node);
			e->pooled_sec = false;
			spin_unlock(&p->lock);
			return e;
		}
	}
	spin_unlock(&p->lock);
	return NULL;
}

/* Park ENT (uncharged class: an io BO the firmware may write again).
 * It keeps its memory and IOVA; a quarantined firmware is the caller's
 * concern (the take side refuses while quarantined). */
static inline void ane_t6021_pool_park_uncharged(struct ane_t6021_pool *p,
						 struct ane_t6021_pool_ent *ent)
{
	ent->pooled_sec = false;
	spin_lock(&p->lock);
	list_add_tail(&ent->node, &p->list);
	spin_unlock(&p->lock);
}

/* Park ENT under the section budget, evicting the oldest budgeted
 * entries (RELEASE really frees each) until it fits. Returns 0 parked,
 * -EINVAL when the size is below the floor or the budget is off, and
 * -ENOSPC when the budget cannot be reached by eviction. On 0 the
 * memory stays allocated and counted; the entry leaves the pool again
 * only through take() or eviction. */
static inline int ane_t6021_pool_park(struct ane_t6021_pool *p,
				      struct ane_t6021_pool_ent *ent,
				      ane_t6021_pool_release_fn release)
{
	struct ane_t6021_pool_ent *old;
	size_t pg = PAGE_ALIGN(ent->size);

	if (!ane_t6021_pool_admits(p, ent->size))
		return -EINVAL;
	while ((size_t)atomic64_read(&p->sec_bytes) + pg >
	       (size_t)p->max_mb << 20) {
		old = ane_t6021_pool_evict_oldest(p);
		if (!old)
			return -ENOSPC;
		release(old);
	}
	ent->pooled_sec = true;
	atomic64_add(pg, &p->sec_bytes);
	spin_lock(&p->lock);
	list_add_tail(&ent->node, &p->list);
	spin_unlock(&p->lock);
	return 0;
}

/* A parked entry of SIZE's page-aligned size, or NULL. Quarantined
 * firmware may still write parked io BOs, so nothing is handed out
 * while quarantined. A budgeted entry is uncharged on the way out: the
 * charge follows the parking, not the buffer. */
static inline struct ane_t6021_pool_ent *
ane_t6021_pool_take(struct ane_t6021_pool *p, size_t size, bool quarantined)
{
	struct ane_t6021_pool_ent *e;

	if (quarantined)
		return NULL;
	spin_lock(&p->lock);
	list_for_each_entry(e, &p->list, node) {
		if (PAGE_ALIGN(e->size) == PAGE_ALIGN(size)) {
			list_del(&e->node);
			if (e->pooled_sec) {
				atomic64_sub(PAGE_ALIGN(e->size),
					     &p->sec_bytes);
				e->pooled_sec = false;
			}
			spin_unlock(&p->lock);
			return e;
		}
	}
	spin_unlock(&p->lock);
	return NULL;
}

static inline s64 ane_t6021_pool_sec_bytes(const struct ane_t6021_pool *p)
{
	return atomic64_read(&p->sec_bytes);
}
