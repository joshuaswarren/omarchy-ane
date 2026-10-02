/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * ane/ane_stats_show.c — sysfs/debugfs show functions shared by
 * ane.ko and ane_t6021.ko. The file operations and device_attribute
 * definitions live here so both drivers expose identical files (same
 * mode, same fields, same parser-friendly header line).
 *
 * Compiled into both out-of-tree ko builds (ane/Makefile adds the
 * source; ane/t6021/Makefile adds it too) and into the in-tree
 * driver pair.
 */

#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/sysfs.h>
#include <linux/export.h>

#include "ane_stats.h"

/*
 * ane_stats sysfs file (coreglass producer contract, no root needed).
 * Mode 0444, ASCII "key value" lines, integers only:
 *   busy_ns <cumulative u64>
 *   jobs    <cumulative u64>
 * Cumulative counters never reset while the device is bound. The
 * sampler (coreglass/coreglass/sampler.py) parses these keys to
 * compute busy = Δbusy_ns / Δt and jobs/s = Δjobs / Δt.
 */
static ssize_t ane_stats_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct ane_stats_counters *ctrs = dev_get_drvdata(dev);

	if (!ctrs)
		return -EINVAL;
	return sysfs_emit(buf, "busy_ns %llu\njobs %llu\n",
			  (unsigned long long)ane_stats_atomic64_read(&ctrs->busy_ns),
			  (unsigned long long)ane_stats_atomic64_read(&ctrs->jobs));
}
DEVICE_ATTR_RO(ane_stats);
EXPORT_SYMBOL_GPL(dev_attr_ane_stats);

/*
 * ane_timeline debugfs file (per-submission ring). Header line gives
 * fields and labels tmst as a raw tick (unit unknown on ane.ko; 0 =
 * unavailable on ane_t6021). Lines are space-separated integers:
 *   seq submit_ns start_ns end_ns tasks rc tmst
 * The reader is built so torn reads are detected via the per-slot
 * seqlock; if seq is odd the line is omitted. Format stays
 * parseable: unknown keys pass through and unknown fields are skipped.
 */
static int ane_timeline_show(struct seq_file *m, void *v)
{
	struct ane_stats_ring *ring = m->private;
	struct ane_stats_ring_entry *ring_slots = ring->slots;
	uint32_t mask = ring->mask;
	uint64_t head = ane_stats_atomic64_read(&ring->head);
	uint64_t start_seq;

	seq_printf(m, "# ane_timeline: seq submit_ns start_ns end_ns tasks rc tmst (tmst raw tick on ane.ko, 0 = unavailable on ane_t6021)\n");
	start_seq = (head > (mask + 1) * 2) ? (head - (mask + 1) * 2) : 0;
	start_seq &= ~(uint64_t)1;
	for (uint64_t s = head & ~(uint64_t)1; s > start_seq; s -= 2) {
		struct ane_stats_ring_entry *e = &ring_slots[s & mask];
		uint64_t seq = ane_stats_atomic64_read_acquire(&e->seq);
		uint64_t submit = ane_stats_atomic64_read(&e->submit_ns);
		uint64_t st = ane_stats_atomic64_read(&e->start_ns);
		uint64_t en = ane_stats_atomic64_read(&e->end_ns);
		uint32_t tasks = ane_stats_atomic_read(&e->tasks);
		uint32_t rc = ane_stats_atomic_read(&e->rc);
		uint64_t tmst = ane_stats_atomic64_read(&e->tmst);

		if (seq != s)
			continue; /* torn write or in-flight; never printed */
		seq_printf(m, "%llu %llu %llu %llu %u %u %llu\n",
			   (unsigned long long)s,
			   (unsigned long long)submit,
			   (unsigned long long)st,
			   (unsigned long long)en,
			   tasks, (unsigned)rc,
			   (unsigned long long)tmst);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ane_timeline);
EXPORT_SYMBOL_GPL(ane_timeline_fops);

/*
 * ane_timeline debugfs file (per-submission ring). Header line gives
 * fields and labels tmst as a raw tick (unit unknown on ane.ko; 0 =
 * unavailable on ane_t6021). Lines are space-separated integers:
 *   seq submit_ns start_ns end_ns tasks rc tmst
 * The reader is built so torn reads are detected via the per-slot
 * seqlock; if seq is odd the line is omitted. Format stays
 * parseable: unknown keys pass through and unknown fields are skipped.
 */
EXPORT_SYMBOL_GPL(ane_timeline_show);