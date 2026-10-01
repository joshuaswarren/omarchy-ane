// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane_dart_probe — read the T6021 ANE DART words that macOS programs, and
 * on request apply the macOS tunables of the two bulk DARTs.
 *
 * apply=0 (default): read only. On dart-ane0 (LLT), dart-ane1 (BRD) and
 *   dart-ane2 (BWR): PARAMS, 0x20c, DIAG_LOCK, 0x220-0x22c, 0x300-0x310,
 *   the m1n1-named PERF words, the per-SID words 0x800-0x83c, and
 *   ENABLE_STREAMS[0] (receipts/2026-10-01-t6021-macos-vs-linux-mmio, E1).
 * apply=1: the 19 macOS RMWs per bulk DART, reg = (reg & ~mask) | value,
 *   in the macOS 13.5 hv trace order (events 2857-2878 and 2887-2908)
 *   without 0x300-0x310. Values from the ADT dart-tunables (E2).
 * apply=2: the same RMWs with value = orig_brd[i] / orig_bwr[i] & mask,
 *   to write back the words read before apply=1.
 *
 * The DART pages belong to apple-dart; this is a second, non-posted
 * mapping. The tunable words are ones apple-dart never writes. Load the
 * probe only while no CALL runs: hold /var/tmp/ane-run.lock.
 *
 * Rules: no access unless ane_sys, ane_cpu (dart-ane1/2), pmp (dart-ane0)
 * and the seven ANE islands read on. The guard runs before the 3 s settle
 * wait and again before every access. Each access is logged before it
 * happens (50 ms drain before a read, 30 ms before a write), so a fault
 * leaves the address as the last netconsole line. A readback that differs
 * from the written value stops the sequence. Nothing stays mapped after
 * init. Output: "ane_dart_probe: 0x<PA> = 0x<value>".
 *
 * Sources (omarchy-ane 12c7fa9 unless named): DART bases and power domains
 * packaging/dt/t602x-ane.dtsi:55-62,112-138 (dart-ane0 in pmp 0x2c8,
 * dart-ane1/2 in ane_cpu 0x2e0), the islands :67-106, the ADT regs :40-44,
 * all checked against the live DT of the test boot. Tunable masks and
 * values: receipts/2026-10-01-t6021-macos-vs-linux-mmio/dart-tunables.tsv.
 * PARAMS and PERF names: m1n1 0b1c9d98b709 proxyclient/m1n1/hw/dart8110.py
 * :11-35,216-234 (the PERF names are marked as guesses there).
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>

#define PS_BASE		0x28e080000ull	/* ANE pmgr PS block (DT "pmgr" reg) */
#define PS_ANE_SYS	0x260
#define PS_PMP		0x2c8		/* dart-ane0 power domain */
#define PS_ANE_CPU	0x2e0		/* dart-ane1/2 power domain */
#define PS_ISLAND0	0x4000		/* ane_sys_mpm, td, base, set1-4 */
#define PS_ISLAND_LAST	0x4030
#define PS_ACTUAL_ON	0xf0		/* ACTUAL [7:4] = 0xf */
#define DART_SPAN	0x4000
#define NDART		3

static const u64 dart_base[NDART] = { 0x285800000ull, 0x285810000ull, 0x285820000ull };

static const u16 rd_off[] = {
	0x000, 0x004, 0x008, 0x00c,
	0x20c, 0x210, 0x220, 0x224, 0x228, 0x22c,
	0x300, 0x304, 0x308, 0x30c, 0x310,
	0x700, 0x704,
	0x720, 0x724, 0x728, 0x72c, 0x730, 0x734, 0x738, 0x73c,
	0x740, 0x744, 0x748, 0x74c, 0x750, 0x754, 0x758, 0x75c,
	0x760, 0x764, 0x768, 0x770, 0x774, 0x778, 0x780, 0x784, 0x788,
	0x800, 0x804, 0x808, 0x80c, 0x810, 0x814, 0x818, 0x81c,
	0x820, 0x824, 0x828, 0x82c, 0x830, 0x834, 0x838, 0x83c,
	0xc00,
};

#define SID_MASK	0x000f007f
static const struct {
	u16 off;
	u32 mask, brd, bwr;
} tun[] = {
	{ 0x20c, 0xff0000b7, 0xe40000b7, 0xe40000b7 },
	{ 0x220, 0x000f0f0f, 0x000f0f0f, 0x000f0f0f },
	{ 0x224, 0x00ffffff, 0x00080808, 0x00080808 },
	{ 0x800, SID_MASK, 0x00060000, 0x00060000 },
	{ 0x804, SID_MASK, 0x00060000, 0x00060000 },
	{ 0x808, SID_MASK, 0x00030040, 0x00010040 },
	{ 0x80c, SID_MASK, 0x00030040, 0x00010040 },
	{ 0x810, SID_MASK, 0x00030040, 0x00010040 },
	{ 0x814, SID_MASK, 0x00010048, 0x00010040 },
	{ 0x818, SID_MASK, 0x00010048, 0x00010040 },
	{ 0x81c, SID_MASK, 0x00010048, 0x00010040 },
	{ 0x820, SID_MASK, 0x00010048, 0x00010040 },
	{ 0x824, SID_MASK, 0x00010048, 0x00010040 },
	{ 0x828, SID_MASK, 0x00010048, 0x00010040 },
	{ 0x82c, SID_MASK, 0x00010048, 0x00010040 },
	{ 0x830, SID_MASK, 0x00010048, 0x00010040 },
	{ 0x834, SID_MASK, 0x00010048, 0x00010040 },
	{ 0x838, SID_MASK, 0x00010048, 0x00010040 },
	{ 0x83c, SID_MASK, 0x00010048, 0x00010040 },
};

static int apply;
module_param(apply, int, 0444);
MODULE_PARM_DESC(apply, "0 read only (default), 1 macOS RMWs on BRD/BWR, 2 write back orig_brd/orig_bwr");
static uint orig_brd[ARRAY_SIZE(tun)], orig_bwr[ARRAY_SIZE(tun)];
static int n_brd, n_bwr;
module_param_array(orig_brd, uint, &n_brd, 0444);
module_param_array(orig_bwr, uint, &n_bwr, 0444);

static bool ps_actual_on(void __iomem *ps, u32 off)
{
	return (readl(ps + off) & PS_ACTUAL_ON) == PS_ACTUAL_ON;
}

static bool power_on(void __iomem *ps)
{
	u32 off;

	if (!ps_actual_on(ps, PS_ANE_SYS) || !ps_actual_on(ps, PS_ANE_CPU) ||
	    !ps_actual_on(ps, PS_PMP))
		return false;
	for (off = PS_ISLAND0; off <= PS_ISLAND_LAST; off += 8)
		if ((readl(ps + off) & 0x3ff) != 0x3ff)
			return false;
	return true;
}

static int power_left(u64 pa, const char *what, unsigned int n)
{
	pr_crit("stopped before %#llx: power left on-state, %u %s\n", pa, n, what);
	return -EAGAIN;
}

static int read_pass(void __iomem *ps, void __iomem *const *dart)
{
	unsigned int d, i, n = 0;

	for (d = 0; d < NDART; d++) {
		for (i = 0; i < ARRAY_SIZE(rd_off); i++) {
			u64 pa = dart_base[d] + rd_off[i];

			pr_crit("read %#llx\n", pa);
			/* ponytail: netconsole is an nbcon kthread and pr_flush() is
			 * not exported; the sleep lets the line leave first. */
			msleep(50);
			if (!power_on(ps))
				return power_left(pa, "read", n);
			pr_crit("%#llx = %#010x\n", pa, readl(dart[d] + rd_off[i]));
			n++;
		}
	}
	pr_crit("done, %u of %zu read, no write\n", n, NDART * ARRAY_SIZE(rd_off));
	return 0;
}

static int rmw_pass(void __iomem *ps, void __iomem *const *dart)
{
	unsigned int d, i, n = 0;

	for (d = 1; d < NDART; d++) {
		for (i = 0; i < ARRAY_SIZE(tun); i++) {
			u64 pa = dart_base[d] + tun[i].off;
			void __iomem *reg = dart[d] + tun[i].off;
			u32 m = tun[i].mask, v, r, w, b;

			if (apply == 1)
				v = d == 1 ? tun[i].brd : tun[i].bwr;
			else
				v = d == 1 ? orig_brd[i] : orig_bwr[i];
			v &= m;
			pr_crit("rmw %#llx\n", pa);
			msleep(50);
			if (!power_on(ps))
				return power_left(pa, "written", n);
			r = readl(reg);
			w = (r & ~m) | v;
			pr_crit("write %#llx %#010x -> %#010x\n", pa, r, w);
			msleep(30);
			if (!power_on(ps))
				return power_left(pa, "written", n);
			writel(w, reg);
			b = readl(reg);
			pr_crit("after %#llx = %#010x%s\n", pa, b, b == w ? "" : " MISMATCH");
			if (b != w) {
				pr_crit("stopped at the first mismatch, %u written\n", n + 1);
				return -EIO;
			}
			n++;
		}
	}
	pr_crit("done, %u of %zu written (apply=%d), readback equal\n",
		n, 2 * ARRAY_SIZE(tun), apply);
	return 0;
}

static int __init dart_probe_init(void)
{
	void __iomem *dart[NDART] = {};
	void __iomem *ps;
	unsigned int d;
	int ret;

	if (!of_machine_is_compatible("apple,t6021"))
		return -ENODEV;
	if (apply < 0 || apply > 2 ||
	    (apply == 2 && (n_brd != ARRAY_SIZE(tun) || n_bwr != ARRAY_SIZE(tun))))
		return -EINVAL;

	ps = ioremap_np(PS_BASE, PS_ISLAND_LAST + 4);
	if (!ps)
		return -ENOMEM;
	if (!power_on(ps)) {
		pr_crit("refused: ane_sys %#010x ane_cpu %#010x pmp %#010x islands %#x %#x %#x %#x %#x %#x %#x, no DART access\n",
			readl(ps + PS_ANE_SYS), readl(ps + PS_ANE_CPU), readl(ps + PS_PMP),
			readl(ps + 0x4000), readl(ps + 0x4008), readl(ps + 0x4010),
			readl(ps + 0x4018), readl(ps + 0x4020), readl(ps + 0x4028),
			readl(ps + 0x4030));
		ret = -EAGAIN;
		goto out;
	}
	msleep(3000);

	for (d = 0; d < NDART; d++) {
		dart[d] = ioremap_np(dart_base[d], DART_SPAN);
		if (!dart[d]) {
			ret = -ENOMEM;
			goto out;
		}
	}
	ret = apply ? rmw_pass(ps, dart) : read_pass(ps, dart);
out:
	for (d = 0; d < NDART; d++)
		if (dart[d])
			iounmap(dart[d]);
	iounmap(ps);
	return ret;
}

static void __exit dart_probe_exit(void)
{
}

module_init(dart_probe_init);
module_exit(dart_probe_exit);

MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("T6021 ANE DART tunables probe (read only unless apply=1/2)");
