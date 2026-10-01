// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Write a list of 32-bit values to ANE DPE tunable registers and read each
 * back. Intended only for the image computed by the offline emulation of
 * macOS AppleT8103PMGR::restoreANEDPE for this die (see ane_dpe_probe).
 * NOT run without the owner's go. Uses the same discipline as ane_dpe_probe:
 * SET word gate (ACTUAL==0xf), ioremap_np, log before every access.
 *
 *  pa_list/val_list : parallel arrays (same length)
 *  restore=1        : on rmmod write the original values back
 *
 * The registers are retained across ANE power-gating only as far as the
 * hardware retains them; reloading `ane` after a gate is not assumed to keep
 * the values - the caller re-reads before measuring.
 */
#include <linux/io.h>
#include <linux/module.h>

#define MAX_LIST 160

static unsigned long ps_phys = 0x23b70c000UL;
module_param(ps_phys, ulong, 0444);

static unsigned long pa_list[MAX_LIST];
static int npa;
module_param_array(pa_list, ulong, &npa, 0444);

static unsigned long val_list[MAX_LIST];
static int nval;
module_param_array(val_list, ulong, &nval, 0444);

static bool restore;
module_param(restore, bool, 0444);

static u32 orig[MAX_LIST];
static int done;

static int rw(unsigned long pa, u32 *oldp, u32 *newp)
{
	void __iomem *m = ioremap_np(pa & ~0xfffUL, 0x1000);

	if (!m)
		return -ENOMEM;
	if (oldp)
		*oldp = readl(m + (pa & 0xfff));
	if (newp) {
		writel(*newp, m + (pa & 0xfff));
		*newp = readl(m + (pa & 0xfff));
	}
	iounmap(m);
	return 0;
}

static int __init ane_dpe_apply_init(void)
{
	void __iomem *ps;
	u32 ps_word, v;
	int i, ret;

	if (!npa || npa != nval)
		return -EINVAL;
	for (i = 0; i < npa; i++)
		if (pa_list[i] & 3)
			return -EINVAL;

	ps = ioremap_np(ps_phys, 4);
	if (!ps)
		return -ENOMEM;
	ps_word = readl(ps);
	iounmap(ps);
	pr_info("ane_dpe_apply: SET+0 = %#x\n", ps_word);
	if (((ps_word >> 4) & 0xf) != 0xf) {
		pr_err("ane_dpe_apply: domain not on, refusing\n");
		return -EIO;
	}
	for (i = 0; i < npa; i++) {
		pr_info("ane_dpe_apply: %#lx <- %#lx\n", pa_list[i], val_list[i]);
		v = (u32)val_list[i];
		ret = rw(pa_list[i], &orig[i], &v);
		if (ret)
			return ret;
		done = i + 1;
		pr_info("ane_dpe_apply: %#lx old %#x new-readback %#x\n",
			pa_list[i], orig[i], v);
	}
	return 0;
}

static void __exit ane_dpe_apply_exit(void)
{
	int i;

	if (!restore)
		return;
	for (i = done - 1; i >= 0; i--) {
		u32 v = orig[i];

		pr_info("ane_dpe_apply: restore %#lx <- %#x\n", pa_list[i], v);
		if (rw(pa_list[i], NULL, &v))
			pr_err("ane_dpe_apply: restore map failed at %#lx\n", pa_list[i]);
	}
}

module_init(ane_dpe_apply_init);
module_exit(ane_dpe_apply_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Apply an emulated macOS ANE DPE tunable image (owner go required)");
