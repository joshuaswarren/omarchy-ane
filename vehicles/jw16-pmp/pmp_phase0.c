// SPDX-License-Identifier: GPL-2.0-only
/*
 * pmp_phase0.c — PMP DVFS route Phase 0 read-only probe (Jw16AnePmp2).
 *
 * ONE-SHOT, READ-ONLY. Every access is announced with a ktime stamp BEFORE
 * it happens (netconsole carries the order); a hang/fault therefore leaves
 * the exact last access as evidence. Read order is safest-first:
 *   1. report SRAM 0x28e3c0000 (driver-proven region, apple-pmp-report bound)
 *   2. mailbox ctrl 0x28ec08110/114 (apple-mailbox bound there)
 *   3. PMP SRAM 0x28e700000 head + bootargs table walk
 *   4. ASC CPU regs 0x28ec00000 +0x44/+0x48 (the one unproven page — LAST)
 * No writes anywhere. Never installed; insmod -> read dmesg -> rmmod.
 */
#include <linux/io.h>
#include <linux/module.h>
#include <linux/time.h>

#define P_REPORT	0x28e3c0000ULL	/* size 0x11000 */
#define P_SRAM		0x28e700000ULL	/* size 0x100000 */
#define P_ASC		0x28ec00000ULL	/* size 0x1000 */
#define P_MBOX		0x28ec08000ULL	/* size 0x1000 */

/* report SRAM offsets (apple_pmp_offsets_t600x + PTD rows at 0x10000+8N) */
#define R_STATUS	0x10
#define R_ACTUAL	0x1000
#define R_TGT_READ	0xf80
#define PTD_PMP_STATUS	(0x10000 + 8 * 1)
#define PTD_DVFS_STATE	(0x10000 + 8 * 8)

/* asc wrap / mailbox */
#define ASC_CPU_CONTROL	0x44
#define ASC_CPU_STATUS	0x48
#define MB_A2I_CTRL	0x110
#define MB_I2A_CTRL	0x114

/* PMP SRAM bootargs */
#define BA_OFFSET	0x22c
#define BA_SIZE		0x230

static void __iomem *m_report, *m_sram, *m_asc, *m_mbox;

#define PRE(addr, off)							\
	pr_info("pmp2: t=%lld ns PRE  read %s @+%#llx\n",		\
		ktime_get_ns(), addr, (unsigned long long)(off))
#define GOT(fmt, val)							\
	pr_info("pmp2: t=%lld ns GOT  " fmt "\n", ktime_get_ns(), (val))

static u64 rd64(void __iomem *b, unsigned long off, const char *what)
{
	u64 v;
	PRE(what, off);
	v = readq(b + off);
	GOT("%s = %#llx", what, v);
	return v;
}

static u32 rd32(void __iomem *b, unsigned long off, const char *what)
{
	u32 v;
	PRE(what, off);
	v = readl(b + off);
	GOT("%s = %#x", what, v);
	return v;
}

static void bootargs_walk(void __iomem *sram)
{
	u32 ptr, size, magic;
	int i;

	ptr = rd32(sram, BA_OFFSET, "bootargs-ptr");
	size = rd32(sram, BA_SIZE, "bootargs-size");
	if (ptr < 0x40 || ptr >= 0x100000 || size < 16 || size > 0x4000) {
		pr_info("pmp2: bootargs pointer/size UNPLAUSIBLE (ptr=%#x size=%#x)\n",
			ptr, size);
		return;
	}
	PRE("sram", (unsigned long)ptr);
	magic = readl(sram + ptr);
	GOT("bootargs magic = %#x (GKTS=0x53544b47)", magic);
	/* driver walk: key u32, size u32, value[size] — bounded to 24 entries */
	for (i = 0; i < 24 && ptr + 8 <= 0x100000; i++) {
		u32 key = readl(sram + ptr);
		u32 vsz = readl(sram + ptr + 4);
		if (vsz > 0x1000) {
			pr_info("pmp2: walk abort at entry %d (vsz=%#x)\n", i, vsz);
			return;
		}
		pr_info("pmp2: bootargs[%d] key=%c%c%c%c size=%#x first=%#x\n",
			i, key & 0xff, (key >> 8) & 0xff, (key >> 16) & 0xff,
			(key >> 24) & 0xff, vsz,
			vsz >= 4 ? readl(sram + ptr + 8) : 0);
		ptr += 8 + vsz;
	}
}

static int __init pmp2_init(void)
{
	pr_info("pmp2: PHASE0 probe start t=%lld\n", ktime_get_ns());

	/* 1. report SRAM (proven) */
	m_report = ioremap_np(P_REPORT, 0x11000);
	if (m_report) {
		rd64(m_report, R_STATUS, "report.status");
		rd64(m_report, R_ACTUAL, "report.actual");
		rd64(m_report, R_TGT_READ, "report.tgt_read");
		rd64(m_report, PTD_PMP_STATUS, "ptd.PMP-STATUS");
		rd64(m_report, PTD_DVFS_STATE, "ptd.DVFS-STATE");
	} else
		pr_info("pmp2: report ioremap FAILED\n");

	/* 2. mailbox ctrl (bound driver's own regs) */
	m_mbox = ioremap_np(P_MBOX, 0x1000);
	if (m_mbox) {
		rd32(m_mbox, MB_A2I_CTRL, "mbox.A2I_CTRL");
		rd32(m_mbox, MB_I2A_CTRL, "mbox.I2A_CTRL");
	} else
		pr_info("pmp2: mbox ioremap FAILED\n");

	/* 3. PMP SRAM */
	m_sram = ioremap_np(P_SRAM, 0x100000);
	if (m_sram) {
		int i;
		for (i = 0; i < 8; i++)
			rd64(m_sram, i * 8, "sram.head");
		bootargs_walk(m_sram);
	} else
		pr_info("pmp2: sram ioremap FAILED\n");

	/* 4. ASC CPU regs (unproven page — deliberately LAST) */
	m_asc = ioremap_np(P_ASC, 0x1000);
	if (m_asc) {
		rd32(m_asc, ASC_CPU_CONTROL, "asc.CPU_CONTROL");
		rd32(m_asc, ASC_CPU_STATUS, "asc.CPU_STATUS");
	} else
		pr_info("pmp2: asc ioremap FAILED\n");

	pr_info("pmp2: PHASE0 probe done t=%lld\n", ktime_get_ns());
	return 0;
}

static void __exit pmp2_exit(void)
{
	if (m_report) iounmap(m_report);
	if (m_mbox) iounmap(m_mbox);
	if (m_sram) iounmap(m_sram);
	if (m_asc) iounmap(m_asc);
	pr_info("pmp2: mappings released\n");
}

module_init(pmp2_init);
module_exit(pmp2_exit);
MODULE_DESCRIPTION("PMP DVFS route Phase 0 read-only probe");
MODULE_LICENSE("Dual MIT/GPL");
