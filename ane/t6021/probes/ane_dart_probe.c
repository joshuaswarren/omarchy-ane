// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane_dart_probe — read the T6021 ANE DART words that macOS programs, and
 * on request apply the macOS tunables of the two bulk DARTs.
 *
 * apply=0 (default): read only. On dart-ane0 (LLT), dart-ane1 (BRD) and
 *   dart-ane2 (BWR): PARAMS, 0x20c, DIAG_LOCK, 0x220-0x22c, 0x300-0x310,
 *   the m1n1-named PERF words, the per-SID words 0x800-0x83c, and
 *   ENABLE_STREAMS[0] (receipts/2026-10-01-t6021-macos-vs-linux-mmio, E1).
 * apply=1: the macOS RMWs on the bulk DARTs, reg = (reg & ~mask) | value,
 *   in the macOS 13.5 hv trace order (events 2857-2878 and 2887-2908)
 *   without 0x300-0x310. Values from the ADT dart-tunables. groups selects
 *   the words: bit 0 0x220/0x224, bit 1 the SID words 0x800-0x83c, bit 2
 *   0x20c (default 7, all 19 per DART; 0 = the stream/flush steps alone).
 * apply=2: the same RMWs with value = orig_brd[i] / orig_bwr[i] & mask,
 *   to write back the words read before apply=1. apply=1 changes only
 *   bits inside each mask, so this is the exact inverse.
 *
 * Writes follow the macOS order (DART init, events 2851-2916: TLB flush,
 * tunables, streams enabled last). Per bulk DART: disable the enabled
 * streams, flush all TLBs, the RMWs, flush again, enable the streams as
 * read. A live RMW without these steps (2f943fe) made both bulk DARTs
 * fault NO PTE (receipts/2026-10-01-t6021-dart-tunables).
 *
 * The DART pages belong to apple-dart; this is a second, non-posted
 * mapping. The tunable words are ones apple-dart never writes. Load the
 * probe only while no CALL runs and no BO is mapped or unmapped: hold
 * /var/tmp/ane-run.lock with no ane-run present.
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
 * :11-35,216-234 (the PERF names are marked as guesses there). TLB_CMD,
 * ERROR, PROTECT, ENABLE/DISABLE_STREAMS, TCR, TTBR: omarchy-linux
 * 57f8f6deaa3a drivers/iommu/apple-dart.c:103-148 (T8110 layout).
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include <linux/iopoll.h>
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

#define DART_TLB_CMD		0x080	/* op [10:8] 0 = flush all */
#define DART_TLB_CMD_BUSY	BIT(31)
#define DART_ERROR		0x100
#define DART_PROTECT		0x200
#define DART_ENABLE_STREAMS	0xc00
#define DART_DISABLE_STREAMS	0xc20
#define DART_TCR0		0x1000
#define DART_TTBR0		0x1400

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
#define G_QOS		BIT(0)	/* 0x220, 0x224 */
#define G_SID		BIT(1)	/* 0x800-0x83c */
#define G_20C		BIT(2)
static const struct {
	u16 off;
	u8 group;
	u32 mask, brd, bwr;
} tun[] = {
	{ 0x20c, G_20C, 0xff0000b7, 0xe40000b7, 0xe40000b7 },
	{ 0x220, G_QOS, 0x000f0f0f, 0x000f0f0f, 0x000f0f0f },
	{ 0x224, G_QOS, 0x00ffffff, 0x00080808, 0x00080808 },
	{ 0x800, G_SID, SID_MASK, 0x00060000, 0x00060000 },
	{ 0x804, G_SID, SID_MASK, 0x00060000, 0x00060000 },
	{ 0x808, G_SID, SID_MASK, 0x00030040, 0x00010040 },
	{ 0x80c, G_SID, SID_MASK, 0x00030040, 0x00010040 },
	{ 0x810, G_SID, SID_MASK, 0x00030040, 0x00010040 },
	{ 0x814, G_SID, SID_MASK, 0x00010048, 0x00010040 },
	{ 0x818, G_SID, SID_MASK, 0x00010048, 0x00010040 },
	{ 0x81c, G_SID, SID_MASK, 0x00010048, 0x00010040 },
	{ 0x820, G_SID, SID_MASK, 0x00010048, 0x00010040 },
	{ 0x824, G_SID, SID_MASK, 0x00010048, 0x00010040 },
	{ 0x828, G_SID, SID_MASK, 0x00010048, 0x00010040 },
	{ 0x82c, G_SID, SID_MASK, 0x00010048, 0x00010040 },
	{ 0x830, G_SID, SID_MASK, 0x00010048, 0x00010040 },
	{ 0x834, G_SID, SID_MASK, 0x00010048, 0x00010040 },
	{ 0x838, G_SID, SID_MASK, 0x00010048, 0x00010040 },
	{ 0x83c, G_SID, SID_MASK, 0x00010048, 0x00010040 },
};

static int apply;
module_param(apply, int, 0444);
MODULE_PARM_DESC(apply, "0 read only (default), 1 macOS RMWs on BRD/BWR, 2 write back orig_brd/orig_bwr");
static uint groups = G_QOS | G_SID | G_20C;
module_param(groups, uint, 0444);
MODULE_PARM_DESC(groups, "words for apply=1/2: bit 0 0x220/0x224, bit 1 SID words, bit 2 0x20c (default 7)");
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

static int power_left(u64 pa)
{
	pr_crit("stopped before %#llx: power left on-state\n", pa);
	return -EAGAIN;
}

/* Log the access, let the line leave, check the power words: a fault then
 * leaves this address as the last netconsole line.
 * ponytail: netconsole is an nbcon kthread and pr_flush() is not exported,
 * so a sleep is the drain. */
static int before(void __iomem *ps, const char *what, u64 pa, unsigned int ms)
{
	pr_crit("%s %#llx\n", what, pa);
	msleep(ms);
	return power_on(ps) ? 0 : power_left(pa);
}

static int read_pass(void __iomem *ps, void __iomem *const *dart)
{
	unsigned int d, i, n = 0;
	int ret;

	for (d = 0; d < NDART; d++) {
		for (i = 0; i < ARRAY_SIZE(rd_off); i++) {
			u64 pa = dart_base[d] + rd_off[i];

			ret = before(ps, "read", pa, 50);
			if (ret)
				return ret;
			pr_crit("%#llx = %#010x\n", pa, readl(dart[d] + rd_off[i]));
			n++;
		}
	}
	pr_crit("done, %u of %zu read, no write\n", n, NDART * ARRAY_SIZE(rd_off));
	return 0;
}

static int flush_all(void __iomem *ps, void __iomem *dart, u64 base)
{
	int ret = before(ps, "flush-all", base + DART_TLB_CMD, 30);
	u32 v;

	if (ret)
		return ret;
	/* ponytail: apple-dart serializes TLB_CMD with its dart->lock, which
	 * is not exported. Safe only while nothing maps or unmaps in the ANE
	 * domain (the ANE lock held, no ane-run). */
	writel(0, dart + DART_TLB_CMD);
	ret = readl_poll_timeout_atomic(dart + DART_TLB_CMD, v, !(v & DART_TLB_CMD_BUSY), 1, 100);
	pr_crit("%#llx flush-all %#010x%s\n", base, v, ret ? " BUSY TIMEOUT" : "");
	return ret;
}

static int rmw(void __iomem *ps, void __iomem *dart, u64 base, unsigned int i, u32 v)
{
	void __iomem *reg = dart + tun[i].off;
	u64 pa = base + tun[i].off;
	u32 m = tun[i].mask, r, w, b;
	int ret = before(ps, "rmw", pa, 50);

	if (ret)
		return ret;
	r = readl(reg);
	w = (r & ~m) | (v & m);
	pr_crit("write %#llx %#010x -> %#010x\n", pa, r, w);
	msleep(30);
	if (!power_on(ps))
		return power_left(pa);
	writel(w, reg);
	b = readl(reg);
	pr_crit("after %#llx = %#010x%s\n", pa, b, b == w ? "" : " MISMATCH");
	return b == w ? 0 : -EIO;
}

/* One bulk DART in the macOS order: streams off, flush, RMWs, flush,
 * streams back on as read. The streams are re-enabled on every path. */
static int quiesced_rmw(void __iomem *ps, void __iomem *dart, u64 base, bool brd,
			unsigned int *n)
{
	unsigned int i;
	u32 en, b, v;
	int ret = before(ps, "state", base, 50);

	if (ret)
		return ret;
	en = readl(dart + DART_ENABLE_STREAMS);
	pr_crit("%#llx enable %#010x tcr0 %#010x ttbr0 %#010x error %#010x protect %#010x\n",
		base, en, readl(dart + DART_TCR0), readl(dart + DART_TTBR0),
		readl(dart + DART_ERROR), readl(dart + DART_PROTECT));
	ret = before(ps, "disable-streams", base + DART_DISABLE_STREAMS, 30);
	if (ret)
		return ret;
	writel(en, dart + DART_DISABLE_STREAMS);
	b = readl(dart + DART_ENABLE_STREAMS);
	pr_crit("%#llx enable after disable %#010x%s\n", base, b, b ? " NOT-ZERO" : "");
	ret = b ? -EIO : flush_all(ps, dart, base);
	for (i = 0; !ret && i < ARRAY_SIZE(tun); i++) {
		if (!(tun[i].group & groups))
			continue;
		if (apply == 1)
			v = brd ? tun[i].brd : tun[i].bwr;
		else
			v = brd ? orig_brd[i] : orig_bwr[i];
		ret = rmw(ps, dart, base, i, v);
		if (!ret)
			(*n)++;
	}
	if (!ret)
		ret = flush_all(ps, dart, base);
	if (before(ps, "enable-streams", base + DART_ENABLE_STREAMS, 30))
		return -EAGAIN;
	writel(en, dart + DART_ENABLE_STREAMS);
	b = readl(dart + DART_ENABLE_STREAMS);
	pr_crit("%#llx enable after enable %#010x%s\n", base, b, b == en ? "" : " MISMATCH");
	if (!ret && b != en)
		ret = -EIO;
	pr_crit("%#llx error %#010x\n", base, readl(dart + DART_ERROR));
	return ret;
}

static int rmw_pass(void __iomem *ps, void __iomem *const *dart)
{
	unsigned int d, n = 0;
	int ret = 0;

	for (d = 1; d < NDART && !ret; d++)
		ret = quiesced_rmw(ps, dart[d], dart_base[d], d == 1, &n);
	pr_crit("%s, %u words written (apply=%d groups=%#x)%s\n", ret ? "stopped" : "done", n,
		apply, groups, ret ? "" : ", readback equal, streams as read");
	return ret;
}

static int __init dart_probe_init(void)
{
	void __iomem *dart[NDART] = {};
	void __iomem *ps;
	unsigned int d;
	int ret;

	if (!of_machine_is_compatible("apple,t6021"))
		return -ENODEV;
	if (apply < 0 || apply > 2 || groups > (G_QOS | G_SID | G_20C) ||
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
