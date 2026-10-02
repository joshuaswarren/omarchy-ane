/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * ane/ane_stats_show.c — ane_timeline debugfs show function shared by
 * ane.ko and ane_t6021.ko. The fops live here so both drivers expose
 * the same file (same mode, same fields, same parser-friendly header
 * line). Compiled into both out-of-tree ko builds (ane/Makefile adds
 * the source; ane/t6021/Makefile adds it too) and into the in-tree
 * driver pair.
 *
 * The ane_stats sysfs attribute is NOT here on purpose: each driver
 * defines its own device_attribute wrapper so the counters are fetched
 * through the real drvdata type and formatted by the typed accessor
 * ane_stats_emit() (ane/include/ane_stats.h). A shared callback would
 * have to cast dev_get_drvdata(), which read the head of struct
 * ane_device as counters on ane.ko (H217 defect 1).
 */

#include <linux/fs.h>
#include <linux/module.h>
#include <linux/seq_file.h>

#include "ane_stats.h"

/*
 * ane_timeline debugfs file (per-submission ring). Header line gives
 * fields and labels tmst as a raw tick (unit unknown on ane.ko; 0 =
 * unavailable on ane_t6021). Lines are space-separated integers:
 *   seq submit_ns start_ns end_ns tasks rc tmst
 * seq is 2 * the submission ticket, committed by ane_stats_complete();
 * an in-flight slot carries an odd seq and is never printed, so torn
 * reads are impossible by construction. Format stays parseable:
 * unknown keys pass through and unknown fields are skipped.
 */
static int ane_timeline_show(struct seq_file *m, void *v)
{
	struct ane_stats_ring *ring = m->private;
	struct ane_stats_ring_entry *ring_slots = ring->slots;
	uint32_t mask = ring->mask;
	uint64_t head = ane_stats_atomic64_read(&ring->head);
	uint64_t live = head < (uint64_t)mask + 1ull ? head : (uint64_t)mask + 1ull;

	seq_printf(m, "# ane_timeline: seq submit_ns start_ns end_ns tasks rc tmst (tmst raw tick on ane.ko, 0 = unavailable on ane_t6021)\n");
	/* Newest first. Submission ticket t lives in slot (t - 1) & mask
	 * and prints once complete() commits seq = 2*t; anything else
	 * (odd in-flight seq, stale slot) is skipped. */
	for (uint64_t i = 0; i < live; i++) {
		uint64_t ticket = head - i;
		struct ane_stats_ring_entry *e =
			&ring_slots[(size_t)(ticket - 1ull) & mask];
		uint64_t seq = ane_stats_atomic64_read_acquire(&e->seq);
		uint64_t submit = ane_stats_atomic64_read(&e->submit_ns);
		uint64_t st = ane_stats_atomic64_read(&e->start_ns);
		uint64_t en = ane_stats_atomic64_read(&e->end_ns);
		uint32_t tasks = ane_stats_atomic_read(&e->tasks);
		uint32_t rc = ane_stats_atomic_read(&e->rc);
		uint64_t tmst = ane_stats_atomic64_read(&e->tmst);

		if (seq != 2ull * ticket)
			continue; /* in flight or stale; never printed */
		seq_printf(m, "%llu %llu %llu %llu %u %u %llu\n",
			   (unsigned long long)(2ull * ticket),
			   (unsigned long long)submit,
			   (unsigned long long)st,
			   (unsigned long long)en,
			   tasks, (unsigned)rc,
			   (unsigned long long)tmst);
	}
	return 0;
}

static int ane_timeline_open(struct inode *inode, struct file *file)
{
	return single_open(file, ane_timeline_show, inode->i_private);
}

/*
 * Non-static on purpose: ane_drv.c and ane_t6021_rtclient_main.c link
 * this TU (same module) and open the file through this symbol. Each
 * driver passes its own &...->stats_ring as the per-file data.
 */
const struct file_operations ane_timeline_fops = {
	.owner		= THIS_MODULE,
	.open		= ane_timeline_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};
