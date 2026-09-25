// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * One-register read of the H13 ANE engine clock word.
 *
 * Approved target: engine+0x1170000 (kext const-pool clock/counter pair
 * CLK2/CTR2, m1n1 hw/ane.py). Not RVBAR (+0x1050000), not CoreSight
 * (+0x1010000), not the ane0 pmgr grant past SET+0x38.
 *
 * Log-then-read: the address is printed and the proven SET word is
 * checked before the single readl. No writes.
 */
#include <linux/io.h>
#include <linux/module.h>

static unsigned long phys = 0x285170000UL;
module_param(phys, ulong, 0444);
MODULE_PARM_DESC(phys, "PA of the one register to read (default engine+0x1170000 on T6001)");

static unsigned long ps_phys = 0x28e08c000UL;
module_param(ps_phys, ulong, 0444);
MODULE_PARM_DESC(ps_phys, "PA of the proven SET word (offset 0 only; never past +0x38)");

static int __init ane_clk_probe_init(void)
{
	void __iomem *ps, *clk;
	phys_addr_t page;
	u32 ps_word, val;
	unsigned long off;

	if (!phys || (phys & 3))
		return -EINVAL;

	pr_info("ane_clk_probe: SET word read next at %#lx (proven +0 only)\n",
		ps_phys);
	ps = ioremap_np(ps_phys, 4);
	if (!ps)
		return -ENOMEM;
	ps_word = readl(ps);
	iounmap(ps);
	pr_info("ane_clk_probe: SET+0 = %#x\n", ps_word);
	if (((ps_word >> 4) & 0xf) != 0xf) {
		pr_err("ane_clk_probe: domain not on, refusing engine read\n");
		return -EIO;
	}

	page = phys & ~0xfffUL;
	off = phys & 0xfffUL;
	pr_info("ane_clk_probe: reading one word at %#lx (page %#llx off %#lx)\n",
		phys, (unsigned long long)page, off);
	clk = ioremap_np(page, 0x1000);
	if (!clk)
		return -ENOMEM;
	val = readl(clk + off);
	pr_info("ane_clk_probe: %#lx = %#x\n", phys, val);
	iounmap(clk);
	return 0;
}

static void __exit ane_clk_probe_exit(void)
{
	pr_info("ane_clk_probe: unloaded\n");
}

module_init(ane_clk_probe_init);
module_exit(ane_clk_probe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read one H13 ANE engine clock register");
