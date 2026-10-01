// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane_afbridge_probe — read-only dump of the 26 T6021 ANE0 AXI2AF bridge
 * registers that macOS programs (AppleT6020PMGR::applyBridgeTunables).
 *
 * The bridge is the first block of the ANE engine window: the macOS 13.5
 * hv trace writes exactly these 26 offsets at 0x284000000 between the
 * ane_sys power-up and the ane_cpu power-up
 * (tools/m2hv_replay-trace-135.txt events 2816-2841).
 *
 * Rules: non-posted mapping only, no write, and no engine read unless
 * ane_sys and the seven compute islands read on. The guard runs before
 * the 3 s settle wait and again before every read. Each line is logged
 * before its read, so a fault leaves the address as the last line.
 * Output: "ane_afbridge_probe: 0x<PA> = 0x<value>", the input format of
 * af_bridge_compare.py check. Nothing stays mapped after init.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>

#define AFB_BASE	0x284000000ull	/* ANE engine base = bridge base */
#define AFB_SPAN	0x1000
#define PS_BASE		0x28e080000ull	/* ANE pmgr PS block (DT "pmgr" reg) */
#define PS_ANE_SYS	0x260
#define PS_ISLAND0	0x4000		/* ane_sys_mpm, td, base, set1-4 */
#define PS_ISLAND_LAST	0x4030
#define PS_ACTUAL_ON	0xf0		/* ACTUAL [7:4] = 0xf */

static const u16 afb_off[] = {
	0x000, 0x00c, 0x010, 0x014, 0x018, 0x01c, 0x020, 0x024, 0x028,
	0x02c, 0x030, 0x034,
	0x108, 0x10c, 0x110, 0x114, 0x118, 0x11c, 0x120, 0x124, 0x128,
	0x12c, 0x130, 0x134,
	0x400, 0xa00,
};

/* ane_sys holds the bridge (macOS programs it right after ane_sys
 * comes up); the island test is the trace_td guard of ane_t6021. */
static bool afb_power_on(void __iomem *ps)
{
	u32 off;

	if ((readl(ps + PS_ANE_SYS) & PS_ACTUAL_ON) != PS_ACTUAL_ON)
		return false;
	for (off = PS_ISLAND0; off <= PS_ISLAND_LAST; off += 8)
		if ((readl(ps + off) & 0x3ff) != 0x3ff)
			return false;
	return true;
}

static int __init afb_init(void)
{
	void __iomem *ps, *afb;
	unsigned int i;
	int ret = 0;

	if (!of_machine_is_compatible("apple,t6021"))
		return -ENODEV;

	ps = ioremap_np(PS_BASE, PS_ISLAND_LAST + 4);
	if (!ps)
		return -ENOMEM;
	if (!afb_power_on(ps)) {
		pr_crit("refused: ane_sys %#010x islands %#x %#x %#x %#x %#x %#x %#x, no engine read\n",
			readl(ps + PS_ANE_SYS), readl(ps + 0x4000), readl(ps + 0x4008),
			readl(ps + 0x4010), readl(ps + 0x4018), readl(ps + 0x4020),
			readl(ps + 0x4028), readl(ps + 0x4030));
		ret = -EAGAIN;
		goto out_ps;
	}
	msleep(3000);

	afb = ioremap_np(AFB_BASE, AFB_SPAN);
	if (!afb) {
		ret = -ENOMEM;
		goto out_ps;
	}
	for (i = 0; i < ARRAY_SIZE(afb_off); i++) {
		u64 pa = AFB_BASE + afb_off[i];

		pr_crit("read %#llx\n", pa);
		/* ponytail: netconsole is an nbcon kthread and pr_flush() is not
		 * exported; 50 ms lets the line leave before the read. The guard
		 * runs after that wait, right before the read. */
		msleep(50);
		if (!afb_power_on(ps)) {
			pr_crit("stopped before %#llx: power left on-state, %u of %zu read\n",
				pa, i, ARRAY_SIZE(afb_off));
			ret = -EAGAIN;
			break;
		}
		pr_crit("%#llx = %#010x\n", pa, readl(afb + afb_off[i]));
	}
	if (!ret)
		pr_crit("done, %zu of %zu read, no write\n",
			ARRAY_SIZE(afb_off), ARRAY_SIZE(afb_off));
	iounmap(afb);
out_ps:
	iounmap(ps);
	return ret;
}

static void __exit afb_exit(void)
{
}

module_init(afb_init);
module_exit(afb_exit);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("T6021 ANE AXI2AF bridge read-only probe");
