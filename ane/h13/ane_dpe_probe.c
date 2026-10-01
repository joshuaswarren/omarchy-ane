// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Read-only readback of the T8103 ANE DPE tunable registers that macOS
 * (AppleT8103PMGR::restoreANEDPE) programs from per-die fuse data, plus the
 * fuse / PMGR words those values are computed from.
 *
 *  q_list  : 64-bit reads (readq), e.g. the 16 fuse qwords
 *  w_list  : 32-bit reads (readl), e.g. PMGR efuse words and DPE registers
 *
 * Same discipline as ane_clk_probe: the ANE_SYS SET word at ps_phys is read
 * first and every read is refused unless ACTUAL (bits 7:4) == 0xf; every
 * address is logged immediately before it is read (a reset leaves the
 * offender as the last log line); ioremap_np only; no writes anywhere.
 * One pass per insmod; the caller chooses which list to pass.
 */
#include <linux/io.h>
#include <linux/module.h>

#define MAX_LIST 128

static unsigned long ps_phys = 0x23b70c000UL;
module_param(ps_phys, ulong, 0444);
MODULE_PARM_DESC(ps_phys, "PA of the proven SET word (offset 0 only)");

static unsigned long q_list[MAX_LIST];
static int nq;
module_param_array(q_list, ulong, &nq, 0444);
MODULE_PARM_DESC(q_list, "PAs to read as 64-bit (8-byte aligned)");

static unsigned long w_list[MAX_LIST];
static int nw;
module_param_array(w_list, ulong, &nw, 0444);
MODULE_PARM_DESC(w_list, "PAs to read as 32-bit (4-byte aligned)");

static char *tag = "";
module_param(tag, charp, 0444);
MODULE_PARM_DESC(tag, "label echoed in every log line (e.g. idle / busy)");

static int __init ane_dpe_probe_init(void)
{
	void __iomem *ps, *m = NULL;
	unsigned long cur_page = ~0UL;
	u32 ps_word;
	int i;

	if (!nq && !nw)
		return -EINVAL;
	for (i = 0; i < nq; i++)
		if (q_list[i] & 7)
			return -EINVAL;
	for (i = 0; i < nw; i++)
		if (w_list[i] & 3)
			return -EINVAL;

	pr_info("ane_dpe_probe[%s]: SET word read next at %#lx\n", tag, ps_phys);
	ps = ioremap_np(ps_phys, 4);
	if (!ps)
		return -ENOMEM;
	ps_word = readl(ps);
	iounmap(ps);
	pr_info("ane_dpe_probe[%s]: SET+0 = %#x\n", tag, ps_word);
	if (((ps_word >> 4) & 0xf) != 0xf) {
		pr_err("ane_dpe_probe[%s]: domain not on, refusing reads\n", tag);
		return -EIO;
	}

	for (i = 0; i < nq + nw; i++) {
		bool wide = i < nq;
		unsigned long pa = wide ? q_list[i] : w_list[i - nq];
		unsigned long page = pa & ~0xfffUL;

		pr_info("ane_dpe_probe[%s]: reading %s at %#lx\n", tag,
			wide ? "q" : "w", pa);
		if (page != cur_page) {
			if (m)
				iounmap(m);
			m = ioremap_np(page, 0x1000);
			if (!m)
				return -ENOMEM;
			cur_page = page;
		}
		if (wide)
			pr_info("ane_dpe_probe[%s]: %#lx = %#llx\n", tag, pa,
				(unsigned long long)readq(m + (pa & 0xfff)));
		else
			pr_info("ane_dpe_probe[%s]: %#lx = %#x\n", tag, pa,
				readl(m + (pa & 0xfff)));
	}
	if (m)
		iounmap(m);
	pr_info("ane_dpe_probe[%s]: done\n", tag);
	return 0;
}

static void __exit ane_dpe_probe_exit(void)
{
}

module_init(ane_dpe_probe_init);
module_exit(ane_dpe_probe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read-only T8103 ANE DPE tunable readback");
