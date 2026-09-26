// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Read-only snapshot of the H13 ANE clock/counter pairs (m1n1 hw/ane.py
 * CLK0-3/CTR0-3, ANE base + 0x1160008..0x1178004) with a monotonic
 * timestamp per word, so two snapshots around one engine submit give
 * counts per nanosecond.
 *
 * Same gate as ane_clk_probe: the proven SET word is read first and the
 * snapshot is refused unless its ACTUAL nibble is 0xf. No writes.
 */
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/module.h>

static unsigned long base = 0x26a000000UL;
module_param(base, ulong, 0444);
MODULE_PARM_DESC(base, "ANE base PA (T8103: 0x26a000000, the ADT reg[0])");

static unsigned long ps_phys = 0x23b70c000UL;
module_param(ps_phys, ulong, 0444);
MODULE_PARM_DESC(ps_phys, "PA of the proven ANE_SYS SET word (offset 0 only)");

static char *tag = "snap";
module_param(tag, charp, 0444);
MODULE_PARM_DESC(tag, "label echoed on every line");

static const struct {
	const char *name;
	u32 off;
} regs[] = {
	{ "CLK0", 0x1160008 }, { "CTR0", 0x116000c },
	{ "CLK1", 0x1168008 }, { "CTR1", 0x116800c },
	{ "CLK2", 0x1170000 }, { "CTR2", 0x1170004 },
	{ "CLK3", 0x1178000 }, { "CTR3", 0x1178004 },
};

static int __init ane_ctr_probe_init(void)
{
	void __iomem *ps, *page;
	u32 ps_word;
	int i;

	ps = ioremap_np(ps_phys, 4);
	if (!ps)
		return -ENOMEM;
	ps_word = readl(ps);
	iounmap(ps);
	pr_info("ane_ctr_probe: %s SET+0 = %#x\n", tag, ps_word);
	if (((ps_word >> 4) & 0xf) != 0xf) {
		pr_err("ane_ctr_probe: domain not on, refusing counter reads\n");
		return -EIO;
	}

	/* Each pair shares one 4 KiB page; map it once, read both words
	 * back to back, log each with its own timestamp. */
	for (i = 0; i < ARRAY_SIZE(regs); i += 2) {
		phys_addr_t pa = base + (regs[i].off & ~0xfffU);
		u64 t0, t1;
		u32 v0, v1;

		page = ioremap_np(pa, 0x1000);
		if (!page)
			return -ENOMEM;
		t0 = ktime_get_ns();
		v0 = readl(page + (regs[i].off & 0xfff));
		t1 = ktime_get_ns();
		v1 = readl(page + (regs[i + 1].off & 0xfff));
		iounmap(page);
		pr_info("ane_ctr_probe: %s %s t=%llu val=%#x\n", tag,
			regs[i].name, t0, v0);
		pr_info("ane_ctr_probe: %s %s t=%llu val=%#x\n", tag,
			regs[i + 1].name, t1, v1);
	}
	return 0;
}

static void __exit ane_ctr_probe_exit(void)
{
}

module_init(ane_ctr_probe_init);
module_exit(ane_ctr_probe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read-only H13 ANE clock/counter snapshot");
