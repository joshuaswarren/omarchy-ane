// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane_clk_twin_probe — read-only Linux twin of macOS window 2
 * (receipts/2026-10-03-ane-macos-window2): the same named pmgr words, in the
 * same order as macos-bundle/ranges-w2.txt (bit i of `mask` = word i), so the
 * two OSes can be compared word by word for the ANE clock operating point.
 *
 * Rules (ane_dcs_ps_probe pattern): non-posted maps only, one exact 4-byte map
 * per selected word, no write, no block dump. It refuses unless ane_sys
 * ACTUAL = 0xf and the seven island PS words read 0x3ff, waits 3 s, and checks
 * the guard again before every sample. The first read of each word is logged
 * ("first read <name> <pa>") with 50 ms for netconsole before the read, so a
 * fault leaves the address as the last line.
 *
 * `mask` (required, set at insmod) selects the words: only words that macOS
 * window 2 read with status ok (analyze_w2.py prints the mask). Init takes the
 * idle sample (seq 0). Each write of N (1-600) to
 * /sys/module/ane_clk_twin_probe/parameters/start takes N samples 100 ms
 * apart. Output: "ane_clk_twin_probe: s=<seq> t=<ns> <name>=0x<v> ...".
 * There is no module_exit: the maps stay until reboot, the only unload.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/of.h>

#define PS_BASE		0x28e080000ull	/* ANE pmgr PS block (DT "pmgr" reg) */
#define PS_ANE_SYS	0x260
#define PS_ISLAND0	0x4000		/* ane_sys_mpm, td, base, set1-4 */
#define PS_ISLAND_LAST	0x4030
#define PS_ACTUAL_ON	0xf0		/* ACTUAL [7:4] = 0xf */
#define MAX_SAMPLES	600

/* ranges-w2.txt order; sources there (AneClockHunt ADT decode). */
static const struct twin_word {
	const char *name;
	u64 pa;
} words[] = {
	{ "fabric-ps",    0x28e20c000ull },	/* pmgr reg[40], read on both OSes */
	{ "dcs-ps",       0x28e20c400ull },
	{ "set-000",      0x28e08c000ull },	/* ane0 SET window, never past +0x38 */
	{ "set-030",      0x28e08c030ull },
	{ "set-034",      0x28e08c034ull },
	{ "set-038",      0x28e08c038ull },
	{ "clk62-33c",    0x29e24033cull },	/* ane0 clock-ids, boot clocks 62-65 */
	{ "clk63-340",    0x29e240340ull },
	{ "clk64-344",    0x29e240344ull },
	{ "clk65-348",    0x29e240348ull },
	{ "pll-ane0",     0x28e0e03b0ull },	/* perf-regs[1] slots (stride inferred) */
	{ "pll-ane1",     0x28e0e03c0ull },
	{ "dev-anesys",   0x28e0e0360ull },
	{ "dev-anecpu",   0x28e0e0370ull },
	{ "ev-clvr-ane",  0x28e0e0b80ull },
	{ "ane0-adclk-0", 0x28e0e8100ull },	/* perf-regs[2] ANE0 trigger slots */
	{ "ane0-adclk-1", 0x28e0e8110ull },
	{ "ane0-dithr-0", 0x28e0e8120ull },
	{ "ane0-dithr-1", 0x28e0e8130ull },
	{ "ane0-ext0-0",  0x28e0e8140ull },
	{ "ane0-ext0-1",  0x28e0e8150ull },
	{ "ane0-ext1-0",  0x28e0e8160ull },
	{ "ane0-ext1-1",  0x28e0e8170ull },
	{ "ane0-ext2-0",  0x28e0e8180ull },
	{ "ane0-ext2-1",  0x28e0e8190ull },
};

#define NWORDS ARRAY_SIZE(words)

static unsigned int mask;
module_param(mask, uint, 0444);
MODULE_PARM_DESC(mask, "Bit i selects word i of ranges-w2.txt; required");

static void __iomem *ps, *map[NWORDS];
static bool seen[NWORDS];
static unsigned int seq;
static DEFINE_MUTEX(twin_lock);

static bool twin_power_on(void)
{
	u32 off;

	if ((readl(ps + PS_ANE_SYS) & PS_ACTUAL_ON) != PS_ACTUAL_ON)
		return false;
	for (off = PS_ISLAND0; off <= PS_ISLAND_LAST; off += 8)
		if ((readl(ps + off) & 0x3ff) != 0x3ff)
			return false;
	return true;
}

/* One sample of every selected word; false if the guard failed first. */
static bool twin_sample(void)
{
	char line[768];
	unsigned int i;
	int n;
	u64 t;

	if (!twin_power_on())
		return false;
	for (i = 0; i < NWORDS; i++) {
		if (!map[i] || seen[i])
			continue;
		pr_crit("first read %s %#llx\n", words[i].name, words[i].pa);
		msleep(50);	/* pr_flush() is not exported; let netconsole send it */
		if (!twin_power_on())
			return false;
		(void)readl(map[i]);
		seen[i] = true;
	}
	t = ktime_get_ns();
	n = scnprintf(line, sizeof(line), "s=%u t=%llu", seq++, t);
	for (i = 0; i < NWORDS; i++)
		if (map[i])
			n += scnprintf(line + n, sizeof(line) - n, " %s=%#010x",
				       words[i].name, readl(map[i]));
	pr_crit("%s\n", line);
	return true;
}

static int twin_start_set(const char *val, const struct kernel_param *kp)
{
	unsigned int n, i;
	int ret;

	ret = kstrtouint(val, 0, &n);
	if (ret)
		return ret;
	if (!n || n > MAX_SAMPLES)
		return -EINVAL;
	mutex_lock(&twin_lock);
	if (!ps) {
		ret = -EAGAIN;	/* init has not mapped the words */
		goto out;
	}
	for (i = 0; i < n; i++) {
		if (i)
			msleep(100);
		if (!twin_sample()) {
			pr_crit("stopped at s=%u: power left on-state, %u of %u read\n",
				seq, i, n);
			ret = -EAGAIN;
			break;
		}
	}
out:
	mutex_unlock(&twin_lock);
	return ret;
}

static const struct kernel_param_ops twin_start_ops = {
	.set = twin_start_set,
};
module_param_cb(start, &twin_start_ops, NULL, 0200);
MODULE_PARM_DESC(start, "Write N (1-600): take N samples 100 ms apart");

static void twin_unmap(void)
{
	unsigned int i;

	for (i = 0; i < NWORDS; i++) {
		if (map[i])
			iounmap(map[i]);
		map[i] = NULL;
	}
	iounmap(ps);
	ps = NULL;
}

static int __init twin_init(void)
{
	unsigned int i;

	BUILD_BUG_ON(NWORDS > 32);
	if (!of_machine_is_compatible("apple,t6021"))
		return -ENODEV;
	if (!mask || mask & ~GENMASK(NWORDS - 1, 0)) {
		pr_crit("refused: mask %#x (need 1..%#lx)\n", mask, GENMASK(NWORDS - 1, 0));
		return -EINVAL;
	}

	ps = ioremap_np(PS_BASE, PS_ISLAND_LAST + 4);
	if (!ps)
		return -ENOMEM;
	if (!twin_power_on()) {
		pr_crit("refused: ane_sys %#010x islands %#x %#x %#x %#x %#x %#x %#x, no read\n",
			readl(ps + PS_ANE_SYS), readl(ps + 0x4000), readl(ps + 0x4008),
			readl(ps + 0x4010), readl(ps + 0x4018), readl(ps + 0x4020),
			readl(ps + 0x4028), readl(ps + 0x4030));
		iounmap(ps);
		ps = NULL;
		return -EAGAIN;
	}
	msleep(3000);

	for (i = 0; i < NWORDS; i++) {
		if (!(mask & BIT(i)))
			continue;
		map[i] = ioremap_np(words[i].pa, 4);
		if (!map[i]) {
			twin_unmap();
			return -ENOMEM;
		}
	}
	pr_crit("mask %#x: %u words mapped\n", mask, hweight32(mask));
	if (!twin_sample()) {
		pr_crit("stopped at init: power left on-state\n");
		twin_unmap();
		return -EAGAIN;
	}
	return 0;
}

module_init(twin_init);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("T6021 ANE clock-state words, read-only twin of macOS window 2");
