// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Engine-busy performance hold: CPU cluster boost, and the T6001 ANE DVFS domain.
 *
 * Bandwidth-bound programs run at a speed set by the CPU cluster
 * p-states. Measured on jwm1 (T8103, 2026-09-25, ane-linux-experiments
 * receipt jwm1-ane-dvfs-boost): with a mostly-sleeping submitter at
 * normal priority, 75 of 489 Qwen decode steps take 100-175 ms instead
 * of 70, every program in a slow step ~2x its normal wall, the
 * submitter's CPU time and run delay unchanged; both clusters pinned
 * at their top p-state (performance governor, or SCHED_FIFO for the
 * submitter, which schedutil boosts the same way) -> 0 of 489. The
 * whole-encoder Parakeet program (140 ms) moves too once the QoS lapses
 * mid-submit: timed from submit start, the hold dropped 100 ms into
 * every encoder run, median 171 ms (140-238); held to completion, 138.0
 * (137.7-138.7), jwm1 2026-09-25. The likely
 * mechanism is the second p-state field apple-soc-cpufreq writes with
 * the cluster p-state on T8103 only (DVFS_CMD PS2, has_ps2); the
 * memory-controller tuning its author named as unimplemented is still
 * absent from the tree. Whatever the field drives, the cure is the
 * cluster p-state, which cpufreq lets a driver request.
 *
 * So hold a min-frequency QoS at the top of every cpufreq policy while a
 * submit runs, and drop it boost_idle_ms after the last one completes.
 * The QoS goes through cpufreq's own constraint object, so it composes
 * with any governor and never touches the DVFS registers.
 */
#include <linux/cpufreq.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/pm_qos.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/workqueue.h>

#include "ane.h"

static unsigned int boost_idle_ms = 100;
module_param(boost_idle_ms, uint, 0644);
MODULE_PARM_DESC(boost_idle_ms,
		 "hold every CPU cluster at its top p-state while a submit runs and this long after the last one completes (0 = off)");

/*
 * T6001 ANE DVFS domain (PMGR domain 8; ADT pmgr reg[113], PA
 * 0x400004000). macOS 25G83 ApplePMGR::_setPerfState(8, s), dtrace on
 * jw16 2026-09-25: after ps_ane_sys powers on it writes DVFS_ON = 1 and
 * a state-0 command; each engine run raises the state to the top of the
 * six-entry ladder (300..1500 MHz), idle drops it to 0, and power-off
 * writes state 0 then DVFS_ON = 0. The command word is
 * SET | prev << 4 | new. Linux never wrote either register (both read
 * 0 on jw16), so the engine ran unmanaged.
 */
#define ANE_DVFS_CMD		0xa00
#define ANE_DVFS_ON		0x2000
#define ANE_DVFS_CMD_SET	BIT(31)

static bool dvfs_ane;
module_param(dvfs_ane, bool, 0444);
MODULE_PARM_DESC(dvfs_ane,
		 "drive the T6001 ANE DVFS domain like macOS: top state while a submit runs, state 0 when idle");

/* Boost lock held. */
static void ane_dvfs_set(struct ane_device *ane, u8 state)
{
	struct ane_boost *b = &ane->boost;

	if (!b->dvfs_online || state == b->dvfs_state)
		return;
	writel(ANE_DVFS_CMD_SET | b->dvfs_state << 4 | state,
	       b->dvfs + ANE_DVFS_CMD);
	b->dvfs_state = state;
}

/* Boost lock held; the ANE is still powered. */
static void ane_dvfs_offline(struct ane_device *ane)
{
	struct ane_boost *b = &ane->boost;

	if (!b->dvfs_online)
		return;
	ane_dvfs_set(ane, 0);
	writel(0, b->dvfs + ANE_DVFS_ON);
	b->dvfs_online = false;
}

/* Lock held. */
static void ane_boost_set(struct ane_device *ane, bool on)
{
	struct ane_boost *b = &ane->boost;
	int cpu, n = 0;

	if (on && boost_idle_ms) {
		for_each_possible_cpu(cpu) {
			struct cpufreq_policy *policy = cpufreq_cpu_get(cpu);
			int err;

			if (!policy)
				continue;
			if (cpu != cpumask_first(policy->related_cpus) ||
			    n >= b->nlegs) {
				cpufreq_cpu_put(policy);
				continue;
			}
			err = freq_qos_add_request(&policy->constraints,
						   &b->legs[n], FREQ_QOS_MIN,
						   policy->cpuinfo.max_freq);
			cpufreq_cpu_put(policy);
			if (err >= 0)
				n++;
		}
		b->held = n;
	} else if (!on) {
		for (n = 0; n < b->held; n++)
			freq_qos_remove_request(&b->legs[n]);
		b->held = 0;
	}
	ane_dvfs_set(ane, on ? b->dvfs_top : 0);
	b->on = on;
}

static void ane_boost_off_work(struct work_struct *work)
{
	struct ane_device *ane =
		container_of(work, struct ane_device, boost.off.work);
	struct ane_boost *b = &ane->boost;

	mutex_lock(&b->lock);
	/* A submit that began or ended after this expiry was armed keeps
	 * the boost on; its own completion re-arms the drop. */
	if (b->on && !b->busy &&
	    time_after_eq(jiffies, b->last_end + msecs_to_jiffies(boost_idle_ms)))
		ane_boost_set(ane, false);
	mutex_unlock(&b->lock);
}

/* Engine lock held: submits never overlap, so busy is a flag. */
void ane_boost_begin(struct ane_device *ane)
{
	struct ane_boost *b = &ane->boost;

	if (!b->legs || (!boost_idle_ms && !b->dvfs))
		return;
	mutex_lock(&b->lock);
	b->busy = true;
	if (!b->on)
		ane_boost_set(ane, true);
	mutex_unlock(&b->lock);
}

void ane_boost_end(struct ane_device *ane)
{
	struct ane_boost *b = &ane->boost;

	if (!b->legs)
		return;
	mutex_lock(&b->lock);
	b->busy = false;
	b->last_end = jiffies;
	if (b->on)
		mod_delayed_work(system_wq, &b->off,
				 msecs_to_jiffies(boost_idle_ms));
	mutex_unlock(&b->lock);
}

static void ane_dvfs_unmap(void *addr)
{
	iounmap(addr);
}

int ane_boost_init(struct ane_device *ane, phys_addr_t dvfs_base, u8 dvfs_top)
{
	struct ane_boost *b = &ane->boost;

	mutex_init(&b->lock);
	INIT_DELAYED_WORK(&b->off, ane_boost_off_work);
	b->nlegs = num_possible_cpus();
	b->legs = kcalloc(b->nlegs, sizeof(*b->legs), GFP_KERNEL);
	if (!b->legs)
		return -ENOMEM;
	if (!dvfs_ane || !dvfs_base)
		return 0;
	/* Non-posted, like the reads that proved this page. A failed map
	 * leaves the driver without DVFS, never unbound. */
	b->dvfs = ioremap_np(dvfs_base, SZ_16K);
	if (b->dvfs && devm_add_action_or_reset(ane->dev, ane_dvfs_unmap, b->dvfs))
		b->dvfs = NULL;
	if (!b->dvfs) {
		dev_err(ane->dev, "ANE-DVFS: map of %pa failed, DVFS off\n",
			&dvfs_base);
		return 0;
	}
	b->dvfs_top = dvfs_top;
	dev_info(ane->dev, "ANE-DVFS: domain at %pa, top state %u\n",
		 &dvfs_base, dvfs_top);
	return 0;
}

/* The ANE partition is powered: bring the DVFS domain online at state 0,
 * as macOS does right after ps_ane_sys comes up, and return to the top
 * state if a submit is holding the boost (recovery power cycle). */
void ane_dvfs_power_on(struct ane_device *ane)
{
	struct ane_boost *b = &ane->boost;

	if (!b->dvfs)
		return;
	mutex_lock(&b->lock);
	writel(1, b->dvfs + ANE_DVFS_ON);
	writel(ANE_DVFS_CMD_SET, b->dvfs + ANE_DVFS_CMD);
	b->dvfs_state = 0;
	b->dvfs_online = true;
	if (b->on)
		ane_dvfs_set(ane, b->dvfs_top);
	dev_info(ane->dev, "ANE-DVFS online: on %#x cmd %#x\n",
		 readl(b->dvfs + ANE_DVFS_ON), readl(b->dvfs + ANE_DVFS_CMD));
	mutex_unlock(&b->lock);
}

/* Before the ANE partition powers down, while it is still on. */
void ane_dvfs_power_off(struct ane_device *ane)
{
	struct ane_boost *b = &ane->boost;

	if (!b->dvfs)
		return;
	mutex_lock(&b->lock);
	ane_dvfs_offline(ane);
	mutex_unlock(&b->lock);
}

void ane_boost_exit(struct ane_device *ane)
{
	struct ane_boost *b = &ane->boost;

	if (!b->legs)
		return;
	cancel_delayed_work_sync(&b->off);
	mutex_lock(&b->lock);
	if (b->on)
		ane_boost_set(ane, false);
	ane_dvfs_offline(ane);
	mutex_unlock(&b->lock);
	kfree(b->legs);
	b->legs = NULL;
}
