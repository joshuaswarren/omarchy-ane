/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * ane/include/ane_stats.h — shared ANE producer-side counters and ring.
 *
 * Used by both out-of-tree drivers (ane.ko, ane_t6021.ko) and by the
 * in-tree Linux port. Compiles in-kernel (with <linux/atomic.h>) and
 * on the host (with GCC __atomic_* builtins or C11 atomics; both work
 * for the unit test on a quiet box). The same code exercises the
 * counter/union/ring logic without touching the device.
 *
 * Contract (maintainer-lead w76 + coreglass docs/DESIGN.md
 * "Producer contract" and "ANE, cheapest first"):
 *   - sysfs under /sys/class/accel/accel<digit>/device/ane_stats,
 *     mode 0444, ASCII "key value" lines, integers only: busy_ns
 *     (cumulative u64), jobs (cumulative u64). busy_ns counts only
 *     nanoseconds the engine was busy (sum/union of submit-to-
 *     completion windows) since the device was bound.
 *   - debugfs ane_timeline: preallocated ring of the last N submissions
 *     with seq, submit_ns, start_ns, end_ns, tasks, rc (and tmst raw
 *     when known). Head counter is atomic; per-slot seqlock so the
 *     reader never blocks the producer and torn reads are detected.
 *   - `stats` module parameter bool, default 1; stats=0 makes the hot
 *     path one predictable branch and skips sysfs/debugfs creation.
 *
 * No allocation, locking, or formatting in the hot path. The sysfs
 * show callback formats in process context; the ring writer commits
 * the slot with a release store, and the reader does an acquire load
 * on begin/end. Counters update with atomics only. busy_ns is the
 * union of busy intervals: for an engine that serializes submissions
 * (ane.ko, behind engine_lock) it equals sum(end - start); for an
 * engine that can run submissions in parallel (ane_t6021) it is the
 * union measure, advanced by max(0, end - max(start, last_end)).
 */

#ifndef __ANE_STATS_H__
#define __ANE_STATS_H__

#include <linux/types.h>

#ifdef __KERNEL__
#include <linux/atomic.h>
#include <linux/ktime.h>
#include <linux/string.h>
#define ane_stats_atomic_u64	atomic64_t
#define ane_stats_atomic_u32	atomic_t
static inline u64 ane_stats_atomic64_read(const atomic64_t *v) { return atomic64_read(v); }
static inline void ane_stats_atomic64_add(u64 i, atomic64_t *v) { atomic64_add(i, v); }
static inline bool ane_stats_atomic64_try_cmpxchg(atomic64_t *v, u64 *old, u64 new) {
	return atomic64_try_cmpxchg(v, old, new);
}
static inline void ane_stats_atomic_set(atomic_t *v, int i) { atomic_set(v, i); }
static inline int ane_stats_atomic_read(const atomic_t *v) { return atomic_read(v); }
static inline u64 ane_stats_atomic64_read_acquire(const atomic64_t *v) {
	return atomic64_read_acquire(v);
}
static inline void ane_stats_atomic64_set_release(atomic64_t *v, u64 i) {
	atomic64_set_release(v, i);
}
static inline void ane_stats_smp_wmb(void) { smp_wmb(); }
static inline u64 ane_stats_now_ns(void) { return ktime_get_ns(); }
#else
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include <string.h>

/*
 * On the host we use GCC __atomic_* builtins (or C11 atomics via
 * stdatomic.h; both compile on a standard toolchain). The atomics are
 * declared on plain u32/u64 fields rather than C11 _Atomic, which lets
 * the same struct compile unmodified on the kernel side (atomic64_t /
 * atomic_t are non-_Atomic opaque types) and on the host without
 * dragging _Atomic semantics through user-facing definitions.
 */
typedef uint64_t ane_stats_atomic_u64;
typedef uint32_t ane_stats_atomic_u32;
static inline uint64_t ane_stats_atomic64_read(const ane_stats_atomic_u64 *v) {
	return __atomic_load_n(v, __ATOMIC_RELAXED);
}
static inline void ane_stats_atomic64_add(uint64_t i, ane_stats_atomic_u64 *v) {
	__atomic_add_fetch(v, i, __ATOMIC_RELAXED);
}
static inline bool ane_stats_atomic64_try_cmpxchg(ane_stats_atomic_u64 *v,
						  uint64_t *old, uint64_t new) {
	return __atomic_compare_exchange_n(v, old, new, 0,
					   __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}
static inline void ane_stats_atomic_set(ane_stats_atomic_u32 *v, uint32_t i) {
	__atomic_store_n(v, i, __ATOMIC_RELAXED);
}
static inline uint32_t ane_stats_atomic_read(const ane_stats_atomic_u32 *v) {
	return __atomic_load_n(v, __ATOMIC_RELAXED);
}
static inline uint64_t ane_stats_atomic64_read_acquire(const ane_stats_atomic_u64 *v) {
	return __atomic_load_n(v, __ATOMIC_ACQUIRE);
}
static inline void ane_stats_atomic64_set_release(ane_stats_atomic_u64 *v, uint64_t i) {
	__atomic_store_n(v, i, __ATOMIC_RELEASE);
}
static inline void ane_stats_smp_wmb(void) { __atomic_thread_fence(__ATOMIC_RELEASE); }
static inline uint64_t ane_stats_now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#define module_param(name, type, perm)
#define MODULE_PARM_DESC(name, desc)
#endif /* __KERNEL__ */

/*
 * Per-device counters. busy_ns holds the cumulative union of busy
 * intervals (nanoseconds the engine was working) since the device was
 * bound. jobs counts completed submissions. last_busy_end is the
 * union helper: the latest end timestamp of a busy window currently
 * incorporated in busy_ns; concurrent submissions add only the part
 * of their window that lies past last_busy_end.
 *
 * Module-level `stats` parameter governs whether these counters and
 * the ring are created and whether the hot path branches out.
 */
struct ane_stats_counters {
	ane_stats_atomic_u64	busy_ns;
	ane_stats_atomic_u64	jobs;
	ane_stats_atomic_u64	last_busy_end;
};

/*
 * Ring slot: seqlock-style per-slot sequence. Writer bumps seq at
 * entry (odd) and again on commit (even); reader loads begin/end
 * inside acquire load and the order relative to begin/end is preserved
 * by release. begin/end 0 means the slot is empty. submit_ns, end_ns,
 * tasks, rc, tmst are the diagnostic fields; tmst is the raw TM tick
 * on ane.ko (unknown unit) and 0 on ane_t6021 (unavailable, marked so
 * in the file header line).
 */
struct ane_stats_ring_entry {
	ane_stats_atomic_u64	seq;		/* even = committed; odd = writing */
	ane_stats_atomic_u64	submit_ns;
	ane_stats_atomic_u64	start_ns;
	ane_stats_atomic_u64	end_ns;
	ane_stats_atomic_u32	tasks;
	ane_stats_atomic_u32	rc;
	ane_stats_atomic_u64	tmst;		/* raw TM tick on ane.ko; 0 = unavailable */
};

/*
 * Ring container. head is the next slot to write (increments mod N).
 * N is a power of two preallocated at probe. slots is the array.
 * Reader takes begin/end under acquire and the seqlock protects
 * against torn writes.
 */
struct ane_stats_ring {
	ane_stats_atomic_u64		head;
	uint32_t			n;	/* power of two */
	uint32_t			mask;	/* n - 1 */
	struct ane_stats_ring_entry	*slots;
};

/*
 * Initialize counters and ring from zero. The ring slots array must
 * be preallocated (probe-time) and zeroed. Named
 * ane_stats_counters_init (not ane_stats_init) so the per-driver
 * glue function (ane_stats_init(struct ane_device *) etc.) can name
 * its own static initializer without conflicting with the prototype.
 */
static inline void ane_stats_counters_init(struct ane_stats_counters *ctrs,
					   struct ane_stats_ring *ring,
					   uint32_t n_shift)
{
	uint32_t n_total = (1u << n_shift);

	memset(ctrs, 0, sizeof(*ctrs));
	memset(ring, 0, sizeof(*ring));
	ring->n = n_total;
	ring->mask = n_total - 1u;
	ane_stats_atomic64_set_release(&ring->head, 0ull);
}

/*
 * Hot-path submission start. Caller holds the device's submission
 * serialization if the engine is single-producer (ane.ko); concurrent
 * drivers (ane_t6021) pass ring/ctrs and rely on the cmpxchg in
 * complete(). Returns the per-slot index (always in [0, N)) for the
 * caller to later call ane_stats_complete() with the same index.
 */
static inline uint32_t ane_stats_begin(struct ane_stats_counters *ctrs,
				       struct ane_stats_ring *ring,
				       uint64_t submit_ns, uint32_t tasks)
{
	uint64_t head = ane_stats_atomic64_read(&ring->head);
	uint32_t idx = (uint32_t)(head & ring->mask);
	struct ane_stats_ring_entry *e = &ring->slots[idx];

	(void)ctrs;
	ane_stats_smp_wmb();
	(void)ane_stats_atomic64_read_acquire(&e->seq); /* pair with reader */
	/* Open a write window: bump seq to an odd value (in flight). */
	ane_stats_atomic64_set_release(&e->seq, (head + 2ull) | 1ull);
	ane_stats_atomic64_set_release(&e->submit_ns, submit_ns);
	ane_stats_atomic64_set_release(&e->start_ns, submit_ns);
	ane_stats_atomic64_set_release(&e->end_ns, submit_ns);
	ane_stats_atomic_set(&e->tasks, tasks);
	ane_stats_atomic_set(&e->rc, (uint32_t)0xFFFFFFFFu); /* sentinel: not done */
	ane_stats_atomic64_set_release(&e->tmst, 0ull);
	ane_stats_atomic64_set_release(&e->seq, (head + 4ull)); /* even = committed */
	/* Reserve the slot: increment after commit so the reader can
	 * distinguish an in-flight write (seq odd on this slot) from a
	 * committed slot waiting to be reused). */
	ane_stats_atomic64_add(1ull, &ring->head);
	return idx;
}

/*
 * Hot-path submission completion. Computes the busy interval and
 * folds it into busy_ns (union rule). Updates the slot's end_ns/rc/
 * tmst. Increments jobs.
 *
 * Busy_ns accounting: each submission's [start_ns, end_ns] window is
 * folded into busy_ns with the union rule
 *       add max(0, end - max(start, last_end))
 *   where last_end is updated to max(last_end, end). On a
 *   single-producer engine start >= last_end always, so the
 *   contribution is end - start.
 */
static inline void ane_stats_complete(struct ane_stats_counters *ctrs,
				      struct ane_stats_ring *ring,
				      uint32_t idx, uint64_t end_ns,
				      uint32_t rc, uint64_t tmst)
{
	struct ane_stats_ring_entry *e = &ring->slots[idx];
	uint64_t start = ane_stats_atomic64_read(&e->start_ns);
	uint64_t prev, add_ns;

	/* Union rule: with last_busy_end = L, submission [s,e] contributes
	 * max(0, e - max(s, L)) and updates L = max(L, e). On a
	 * single-producer engine s >= L always, so the contribution is
	 * e - s. */
	prev = ane_stats_atomic64_read(&ctrs->last_busy_end);
	for (;;) {
		uint64_t lo = (start > prev) ? start : prev;
		uint64_t hi = end_ns;
		uint64_t nxt = (hi > prev) ? hi : prev;

		add_ns = (hi > lo) ? (hi - lo) : 0ull;
		if (ane_stats_atomic64_try_cmpxchg(&ctrs->last_busy_end, &prev, nxt))
			break;
	}
	if (add_ns)
		ane_stats_atomic64_add(add_ns, &ctrs->busy_ns);
	ane_stats_atomic64_add(1ull, &ctrs->jobs);

	/* Commit ring slot. */
	ane_stats_smp_wmb();
	ane_stats_atomic64_set_release(&e->end_ns, end_ns);
	ane_stats_atomic_set(&e->rc, rc);
	ane_stats_atomic64_set_release(&e->tmst, tmst);
	ane_stats_smp_wmb();
	/* Final even seq at the slot's completion; reader sees a
	 * snapshot at (h * 2 - 1) where h is the slot's reserved head. */
	ane_stats_atomic64_set_release(&e->seq, ane_stats_atomic64_read(&ring->head) * 2ull);
}

#define ANE_STATS_RING_ORDER_DEFAULT 8  /* 256 slots */
#define ANE_STATS_RING_ORDER_MAX     10 /* 1024 slots */

#endif /* __ANE_STATS_H__ */