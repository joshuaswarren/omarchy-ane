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
 *     nanoseconds the engine was busy (union of submit-to-completion
 *     windows) since the device was bound, and advances while work is
 *     in flight: the show callback adds the open period's live tail.
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
 * union of busy intervals: a busy period opens when the first
 * submission lands on an idle engine and closes when the last one
 * completes, and its whole span folds once at close; overlapping
 * submissions share the period, so a serialized engine (ane.ko,
 * behind engine_lock) reports sum(end - start) and a parallel engine
 * (ane_t6021) reports the union, never the sum.
 */

#ifndef __ANE_STATS_H__
#define __ANE_STATS_H__

#include <linux/types.h>

#ifdef __KERNEL__
#include <linux/atomic.h>
#include <linux/ktime.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#define ane_stats_atomic_u64	atomic64_t
#define ane_stats_atomic_u32	atomic_t
static inline u64 ane_stats_atomic64_read(const atomic64_t *v) { return atomic64_read(v); }
static inline void ane_stats_atomic64_add(u64 i, atomic64_t *v) { atomic64_add(i, v); }
static inline u64 ane_stats_atomic64_fetch_add(u64 i, atomic64_t *v) { return atomic64_fetch_add(i, v); }
static inline bool ane_stats_atomic64_try_cmpxchg(atomic64_t *v, u64 *old, u64 new) {
	return atomic64_try_cmpxchg(v, old, new);
}
static inline void ane_stats_atomic_set(atomic_t *v, int i) { atomic_set(v, i); }
static inline void ane_stats_atomic_set_release(atomic_t *v, u32 i) {
	atomic_set_release(v, (int)i);
}
static inline int ane_stats_atomic_read(const atomic_t *v) { return atomic_read(v); }
static inline u32 ane_stats_atomic_cmpxchg(atomic_t *v, u32 old, u32 new) {
	return (u32)atomic_cmpxchg(v, (int)old, (int)new);
}
static inline bool ane_stats_atomic_try_cmpxchg(atomic_t *v, u32 *old, u32 new) {
	return atomic_try_cmpxchg(v, (int *)old, (int)new);
}
static inline u32 ane_stats_atomic_read_acquire(const atomic_t *v) {
	return (u32)atomic_read_acquire(v);
}
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
#include <stdio.h>
#include <time.h>
#include <string.h>

/* ane_stats_emit writes at most this many bytes (kernel side sysfs_emit
 * asserts a page buffer; the bound documents the format size: two u64
 * keys can reach 8 + 20 + 1 + 5 + 20 + 1 = 55 bytes). */
#define ANE_STATS_EMIT_MAX 64

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
static inline uint64_t ane_stats_atomic64_fetch_add(uint64_t i, ane_stats_atomic_u64 *v) {
	return __atomic_fetch_add(v, i, __ATOMIC_RELAXED);
}
static inline bool ane_stats_atomic64_try_cmpxchg(ane_stats_atomic_u64 *v,
						  uint64_t *old, uint64_t new) {
	return __atomic_compare_exchange_n(v, old, new, 0,
					   __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}
static inline void ane_stats_atomic_set(ane_stats_atomic_u32 *v, uint32_t i) {
	__atomic_store_n(v, i, __ATOMIC_RELAXED);
}
static inline void ane_stats_atomic_set_release(ane_stats_atomic_u32 *v,
						uint32_t i) {
	__atomic_store_n(v, i, __ATOMIC_RELEASE);
}
static inline uint32_t ane_stats_atomic_read(const ane_stats_atomic_u32 *v) {
	return __atomic_load_n(v, __ATOMIC_RELAXED);
}
static inline uint32_t ane_stats_atomic_cmpxchg(ane_stats_atomic_u32 *v,
						uint32_t old, uint32_t new) {
	__atomic_compare_exchange_n(v, &old, new, 0,
				    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
	return old;
}
static inline bool ane_stats_atomic_try_cmpxchg(ane_stats_atomic_u32 *v,
						uint32_t *old, uint32_t new) {
	return __atomic_compare_exchange_n(v, old, new, 0,
					   __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
}
static inline uint32_t ane_stats_atomic_read_acquire(const ane_stats_atomic_u32 *v) {
	return __atomic_load_n(v, __ATOMIC_ACQUIRE);
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
 * Per-device counters. busy_ns holds the busy time folded from closed
 * busy periods: a period opens when the first submission arrives on an
 * idle engine and closes when the last one completes, and its whole
 * span folds once at close. Overlapping submissions share the period,
 * so busy_ns is the union of the submit-to-completion intervals, not
 * their sum. The show callback adds the live tail of the still-open
 * period, so the reported value advances while engine work is in
 * flight. jobs counts completed submissions. last_busy_end is the open
 * period's start timestamp; inflight is the number of in-flight
 * submissions, with ANE_STATS_INFLIGHT_TRANS as the transient close/
 * open sentinel that keeps a fold and a period start from interleaving.
 *
 * Module-level `stats` parameter governs whether these counters and
 * the ring are created and whether the hot path branches out.
 */
#define ANE_STATS_INFLIGHT_TRANS 0xFFFFFFFFu

struct ane_stats_counters {
	ane_stats_atomic_u64	busy_ns;
	ane_stats_atomic_u64	jobs;
	ane_stats_atomic_u64	last_busy_end;
	ane_stats_atomic_u64	max_end;
	ane_stats_atomic_u32	inflight;
};

/* max_end tracks the latest completion sample fed to complete(); the
 * draining completion folds against max(end_ns, max_end) so a stale
 * sample on the last completer cannot truncate the period. */
static inline void ane_stats_atomic64_max(uint64_t i,
					  ane_stats_atomic_u64 *v)
{
	uint64_t old = ane_stats_atomic64_read(v);

	while (i > old && !ane_stats_atomic64_try_cmpxchg(v, &old, i))
		;
}

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
 * drivers (ane_t6021) pass ring/ctrs and rely on the cmpxchg loops.
 * Returns the submission ticket (head + 1, starting at 1) for the
 * caller to later call ane_stats_complete() with. The ticket, not a
 * slot index, identifies the submission: slots are shared after wrap,
 * tickets are not.
 *
 * Counter side: the first submission onto an idle engine opens a busy
 * period by latching last_busy_end = submit_ns under the transition
 * sentinel; later overlapping submissions only increment inflight.
 * The sentinel window is a handful of instructions, so the bounded
 * cmpxchg retries never observe a half-open period.
 */
static inline uint64_t ane_stats_begin(struct ane_stats_counters *ctrs,
				       struct ane_stats_ring *ring,
				       uint64_t submit_ns, uint32_t tasks)
{
	uint64_t ticket = ane_stats_atomic64_fetch_add(1ull, &ring->head) + 1ull;
	struct ane_stats_ring_entry *e =
		&ring->slots[(size_t)(ticket - 1ull) & ring->mask];
	/* A period never starts before the previous one folded: a
	 * caller sample can be stale (taken before the transition),
	 * so the latch is max(submit_ns, max_end). */
	uint64_t latch = ane_stats_atomic64_read(&ctrs->max_end);
	uint32_t cur;

	if (submit_ns > latch)
		latch = submit_ns;
	cur = ane_stats_atomic_read(&ctrs->inflight);

	for (;;) {
		if (cur == ANE_STATS_INFLIGHT_TRANS) {
			cur = ane_stats_atomic_read(&ctrs->inflight);
		} else if (!cur) {
			if (ane_stats_atomic_cmpxchg(&ctrs->inflight, 0u,
						     ANE_STATS_INFLIGHT_TRANS) != 0u) {
				cur = ane_stats_atomic_read(&ctrs->inflight);
				continue;
			}
			ane_stats_atomic64_set_release(&ctrs->last_busy_end,
						       latch);
			ane_stats_atomic_set_release(&ctrs->inflight, 1u);
			break;
		} else if (ane_stats_atomic_try_cmpxchg(&ctrs->inflight, &cur,
							cur + 1u)) {
			break;
		}
	}

	ane_stats_smp_wmb();
	(void)ane_stats_atomic64_read_acquire(&e->seq); /* pair with reader */
	/* In flight: seq stays odd (2*ticket - 1) until complete()
	 * commits the even final value 2*ticket. The reader only prints
	 * even seqs it can match, so an unfinished submission never
	 * prints and a torn write is never visible. */
	ane_stats_atomic64_set_release(&e->seq, 2ull * ticket - 1ull);
	ane_stats_atomic64_set_release(&e->submit_ns, submit_ns);
	/* start_ns is the busy-period start this submission is counted
	 * from (the latch), not the caller's sample: a sample taken
	 * before the transition can predate period entry, and the
	 * union reference must use consumed values. */
	ane_stats_atomic64_set_release(&e->start_ns,
				       ane_stats_atomic64_read(&ctrs->last_busy_end));
	ane_stats_atomic64_set_release(&e->end_ns, submit_ns);
	ane_stats_atomic_set(&e->tasks, tasks);
	ane_stats_atomic_set(&e->rc, (uint32_t)0xFFFFFFFFu); /* sentinel: not done */
	ane_stats_atomic64_set_release(&e->tmst, 0ull);
	return ticket;
}
/*
 * Hot-path submission completion. The last submission out of a busy
 * period closes it: under the transition sentinel it folds the whole
 * period span [last_busy_end, end_ns] into busy_ns before reopening
 * the counter, so no reader can see a period both live and folded.
 * Overlapping submissions share the period, so busy_ns is the union
 * of the submit-to-completion intervals, not their sum. Updates the
 * slot's end_ns/rc/tmst and increments jobs.
 */
static inline void ane_stats_complete(struct ane_stats_counters *ctrs,
				      struct ane_stats_ring *ring,
				      uint64_t ticket, uint64_t end_ns,
				      uint32_t rc, uint64_t tmst)
{
	struct ane_stats_ring_entry *e =
		&ring->slots[(size_t)(ticket - 1ull) & ring->mask];
	uint32_t cur = ane_stats_atomic_read(&ctrs->inflight);
	uint64_t fold_end = 0ull;

	/* Feed this completion's end sample before the transition: a
	 * drainer can only observe inflight == 1 after every other
	 * completer has fed, so the fold below sees the whole period. */
	ane_stats_atomic64_max(end_ns, &ctrs->max_end);

	for (;;) {
		if (cur == ANE_STATS_INFLIGHT_TRANS) {
			cur = ane_stats_atomic_read(&ctrs->inflight);
		} else if (cur > 1u) {
			if (ane_stats_atomic_try_cmpxchg(&ctrs->inflight, &cur,
							 cur - 1u))
				break;
		} else if (!cur) {
			/* Unbalanced complete (cannot happen with the
			 * documented one-begin-per-complete pairing):
			 * count the job, fold nothing, never hang. */
			break;
		} else if (ane_stats_atomic_cmpxchg(&ctrs->inflight, 1u,
						    ANE_STATS_INFLIGHT_TRANS) == 1u) {
			uint64_t s = ane_stats_atomic64_read(&ctrs->last_busy_end);

			/* The period ends at the latest completion sample
			 * in it, not at this caller's (possibly stale)
			 * sample: every member fed max_end before the
			 * drain could observe inflight == 1. */
			fold_end = ane_stats_atomic64_read(&ctrs->max_end);
			if (fold_end < end_ns)
				fold_end = end_ns;
			if (fold_end > s)
				ane_stats_atomic64_add(fold_end - s, &ctrs->busy_ns);
			ane_stats_atomic_set_release(&ctrs->inflight, 0u);
			break;
		} else {
			cur = ane_stats_atomic_read(&ctrs->inflight);
		}
	}
	ane_stats_atomic64_add(1ull, &ctrs->jobs);

	/* Commit ring slot with the even final seq 2*ticket. The value
	 * must not depend on the current ring->head: concurrent
	 * submissions (ane_t6021) advance it, and a head-derived seq
	 * would mislabel the slot. The drainer records the consumed
	 * fold end, not its own sample, so the slot union equals
	 * busy_ns exactly. */
	ane_stats_smp_wmb();
	if (fold_end)
		ane_stats_atomic64_set_release(&e->end_ns, fold_end);
	else
		ane_stats_atomic64_set_release(&e->end_ns, end_ns);
	ane_stats_atomic_set(&e->rc, rc);
	ane_stats_atomic64_set_release(&e->tmst, tmst);
	ane_stats_smp_wmb();
	ane_stats_atomic64_set_release(&e->seq, 2ull * ticket);
}

#define ANE_STATS_RING_ORDER_DEFAULT 8  /* 256 slots */
#define ANE_STATS_RING_ORDER_MAX     10 /* 1024 slots */

/*
 * Accounting rule (coreglass producer contract): jobs counts completed
 * submissions, one begin/complete pair per engine submission — one per
 * ANE_SUBMIT on ane.ko (so libane ane_exec is 1 job and ane_exec_loop
 * with N iterations is N jobs) and one per firmware PROCEDURE_CALL
 * (CSNE_CMD_PROCEDURE_CALL) on ane_t6021.ko: that opcode is the only
 * engine work on the shared ane_rtclient_command path. The control-
 * plane exchanges that ride the same function — LOAD_PROGRAM,
 * CREATE_PROCESS, CH_PROPERTY_WRITE, install-time CONFIG_GET — and the
 * boot transport are not engine submissions and are not counted; jobs
 * must match the number of engine calls the workload made (+-1 at the
 * sampler boundary). A submission that completes with an error still
 * completes its begin/complete pair, so the counter stays balanced.
 *
 * Typed sysfs formatter for the ane_stats attribute. The per-driver
 * show callbacks fetch the counters from their real drvdata type
 * (struct ane_device * on ane.ko, struct ane_rtclient * on
 * ane_t6021.ko) and pass &...->stats_ctrs here. The formatter never
 * sees the device pointer, so the drvdata type confusion that shipped
 * in the first round (reading the head of ane_device as counters)
 * cannot compile again: there is no cast to remove.
 */
/*
 * Consistent counter snapshot across the tiny close/open transition:
 * the transition sentinel is never accepted, and (inflight, busy_ns)
 * must read back unchanged, so a value is only reported between
 * transitions. The value is busy_ns plus the live tail of the
 * still-open period, so it advances while engine work is in flight.
 * Over the timeline the reported value is non-decreasing: folds only
 * add, and at a close the live tail equals the fold.
 */
static inline uint64_t ane_stats_snapshot(const struct ane_stats_counters *ctrs)
{
	uint64_t raw, busy = 0, now, s = 0;
	uint32_t inflight;

	for (;;) {
		inflight = ane_stats_atomic_read_acquire(&ctrs->inflight);
		if (inflight == ANE_STATS_INFLIGHT_TRANS)
			continue; /* close/open in progress: spin it out */
		raw = ane_stats_atomic64_read(&ctrs->busy_ns);
		if (inflight) {
			s = ane_stats_atomic64_read(&ctrs->last_busy_end);
		}
		now = ane_stats_now_ns();
		/* Recheck after the timestamp: a fold that landed before
		 * `now` changes busy_ns and retries; one that lands
		 * after is genuinely later than `now`, so the value is
		 * exact, never an overshoot. */
		if (ane_stats_atomic64_read(&ctrs->busy_ns) != raw ||
		    ane_stats_atomic_read(&ctrs->inflight) != inflight ||
		    (inflight &&
		     ane_stats_atomic64_read(&ctrs->last_busy_end) != s))
			continue;
		busy = raw;
		if (inflight && now > s)
			busy += now - s;
		break;
	}
	return busy;
}

#ifdef __KERNEL__
static inline ssize_t ane_stats_emit(char *buf,
				     const struct ane_stats_counters *ctrs)
{
	return sysfs_emit(buf, "busy_ns %llu\njobs %llu\n",
			  (unsigned long long)ane_stats_snapshot(ctrs),
			  (unsigned long long)ane_stats_atomic64_read(&ctrs->jobs));
}
#else
static inline int ane_stats_emit(char *buf,
				 const struct ane_stats_counters *ctrs)
{
	return snprintf(buf, ANE_STATS_EMIT_MAX,
			"busy_ns %llu\njobs %llu\n",
			(unsigned long long)ane_stats_snapshot(ctrs),
			(unsigned long long)ane_stats_atomic64_read(&ctrs->jobs));
}
#endif /* __KERNEL__ */

#endif /* __ANE_STATS_H__ */