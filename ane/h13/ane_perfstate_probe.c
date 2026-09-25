// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Bounded probe for the T8103 ANE performance state.
 *
 * The ANE perf domain (pmgr "perf-domains" index 8, name ANE) selects
 * perf-regs block 0. That block is reg[1] + 0x34000 = PA 0x23d2b4000,
 * size 0x100, which is inside the range the ADT itself lists for pmgr
 * (reg[1] = 0x23d280000, size 0x74000). The ANE clock entry in the same
 * table (perf index 4 of block 0) lands at +0x140 of that block.
 *
 * Asahi's apple-pmgr-misc drives the same kind of register: the desired
 * state is bits 3:0 of the word, and the granted state is read back in
 * bits 7:4 (same layout as the power-state SET word). The ANE ladder is
 * voltage-states8, 12 steps, 432 to 1464 MHz, so step 11 is the top.
 *
 * Default is one read and no write. request=11 writes the desired field
 * once, waits for the granted field to match, and restores the saved word
 * on unload. A grant that never arrives is put back before the module
 * fails, so a rejected request cannot stick.
 */
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>

#define ANE_PSTATE_DESIRED	GENMASK(3, 0)
#define ANE_PSTATE_ACTUAL	GENMASK(7, 4)
#define ANE_PSTATE_STEPS	12

static unsigned long phys = 0x23d2b4140UL;
module_param(phys, ulong, 0444);
MODULE_PARM_DESC(phys, "PA of the ANE perf-state word (default 0x23d2b4140)");

static int request = -1;
module_param(request, int, 0444);
MODULE_PARM_DESC(request, "perf step to request, 0-11; -1 reads only");

static u32 saved;
static void __iomem *reg;
static bool armed;

static u32 read_actual(u32 word)
{
	return FIELD_GET(ANE_PSTATE_ACTUAL, word);
}

static int __init ane_perfstate_probe_init(void)
{
	u32 before, after;
	int i;

	if (phys & 3)
		return -EINVAL;
	if (request < -1 || request >= ANE_PSTATE_STEPS)
		return -EINVAL;

	pr_info("ane_perfstate: reading %#lx\n", phys);
	reg = ioremap_np(phys & ~0xfffUL, 0x1000);
	if (!reg)
		return -ENOMEM;

	before = readl(reg + (phys & 0xfffUL));
	pr_info("ane_perfstate: before %#x desired %u actual %u\n", before,
		(u32)FIELD_GET(ANE_PSTATE_DESIRED, before), read_actual(before));

	if (request < 0) {
		iounmap(reg);
		reg = NULL;
		return 0;
	}

	saved = before;
	after = (before & ~ANE_PSTATE_DESIRED) |
		FIELD_PREP(ANE_PSTATE_DESIRED, request);
	pr_info("ane_perfstate: requesting step %d (word %#x)\n", request, after);
	writel(after, reg + (phys & 0xfffUL));

	for (i = 0; i < 200; i++) {
		after = readl(reg + (phys & 0xfffUL));
		if (read_actual(after) == (u32)request) {
			armed = true;
			pr_info("ane_perfstate: granted %#x after %d ms\n",
				after, i);
			return 0;
		}
		usleep_range(1000, 1500);
	}

	pr_err("ane_perfstate: step %d not granted (word %#x), restoring %#x\n",
	       request, after, saved);
	writel(saved, reg + (phys & 0xfffUL));
	iounmap(reg);
	reg = NULL;
	return -ETIMEDOUT;
}

static void __exit ane_perfstate_probe_exit(void)
{
	u32 word;

	if (!reg)
		return;
	if (armed) {
		writel(saved, reg + (phys & 0xfffUL));
		usleep_range(2000, 3000);
		word = readl(reg + (phys & 0xfffUL));
		pr_info("ane_perfstate: restored %#x (saved %#x)\n", word, saved);
	}
	iounmap(reg);
}

module_init(ane_perfstate_probe_init);
module_exit(ane_perfstate_probe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read or request the T8103 ANE perf state, one word");
