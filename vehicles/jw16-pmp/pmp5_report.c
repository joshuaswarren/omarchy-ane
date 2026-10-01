// SPDX-License-Identifier: GPL-2.0-only
/*
 * pmp5_report.c — Jw16AnePmp5 Phase 1d/2 read-only report-SRAM reader.
 * ONE-SHOT, READ-ONLY. pmp_phase0b (pmp2b) copy with the three SOC-DEV PTD
 * rows added (PS-REQ 0xf8, PS-ACK 0x100, SOC-DEV-DVFS 0x108) that the
 * Phase-2 protocol acknowledges against. Same access class, same order.
 * insmod -> read dmesg -> rmmod. No writes anywhere.
 */
#include <linux/io.h>
#include <linux/module.h>
#include <linux/time.h>

#define P_REPORT	0x28e3c0000ULL	/* size 0x11000 */
#define P_SRAM		0x28e700000ULL	/* size 0x100000 */
#define P_ASC		0x28ec00000ULL	/* size 0x1000 */
#define P_MBOX		0x28ec08000ULL	/* size 0x1000 */

#define R_STATUS	0x10
#define R_ACTUAL	0x1000
#define R_TGT_READ	0xf80
#define PTD_PMP_STATUS	(0x10000 + 8 * 1)
#define PTD_DVFS_STATE	(0x10000 + 8 * 8)
#define PTD_PS_REQ	(0x10000 + 8 * 31)
#define PTD_PS_ACK	(0x10000 + 8 * 32)
#define PTD_SOC_DEV_DVFS (0x10000 + 8 * 33)

#define ASC_CPU_CONTROL	0x44
#define ASC_CPU_STATUS	0x48
#define MB_A2I_CTRL	0x110
#define MB_I2A_CTRL	0x114

static void __iomem *m_report, *m_sram, *m_asc, *m_mbox;

#define PRE(addr, off)							\
	pr_info("pmp5rep: t=%lld ns PRE  read %s @+%#llx\n",		\
		ktime_get_ns(), addr, (unsigned long long)(off))
#define GOT(fmt, ...)							\
	pr_info("pmp5rep: t=%lld ns GOT  " fmt "\n", ktime_get_ns(), ##__VA_ARGS__)

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

static int __init pmp5rep_init(void)
{
	/* 1. report SRAM (proven) */
	m_report = ioremap_np(P_REPORT, 0x11000);
	if (m_report) {
		rd64(m_report, R_STATUS, "report.status");
		rd64(m_report, R_ACTUAL, "report.actual");
		rd64(m_report, R_TGT_READ, "report.tgt_read");
		rd64(m_report, PTD_PMP_STATUS, "ptd.PMP-STATUS");
		rd64(m_report, PTD_DVFS_STATE, "ptd.DVFS-STATE");
		rd64(m_report, PTD_PS_REQ, "ptd.SOC-DEV-PS-REQ");
		rd64(m_report, PTD_PS_ACK, "ptd.SOC-DEV-PS-ACK");
		rd64(m_report, PTD_SOC_DEV_DVFS, "ptd.SOC-DEV-DVFS");
		iounmap(m_report);
	} else {
		pr_info("pmp5rep: report SRAM ioremap FAILED\n");
	}
	/* 2. mailbox ctrl (proven) */
	m_mbox = ioremap_np(P_MBOX, 0x1000);
	if (m_mbox) {
		rd32(m_mbox, MB_A2I_CTRL, "mbox.A2I_CTRL");
		rd32(m_mbox, MB_I2A_CTRL, "mbox.I2A_CTRL");
		iounmap(m_mbox);
	}
	/* 3. PMP SRAM head (proven on this chain) */
	m_sram = ioremap_np(P_SRAM, 0x1000);
	if (m_sram) {
		rd32(m_sram, 0x0, "sram.head");
		iounmap(m_sram);
	}
	/* 4. ASC CPU regs (the unproven page — LAST) */
	m_asc = ioremap_np(P_ASC, 0x1000);
	if (m_asc) {
		rd32(m_asc, ASC_CPU_CONTROL, "asc.CPU_CONTROL");
		rd32(m_asc, ASC_CPU_STATUS, "asc.CPU_STATUS");
		iounmap(m_asc);
	}
	pr_info("pmp5rep: probe done\n");
	return 0;
}

static void __exit pmp5rep_exit(void)
{
	pr_info("pmp5rep: unloaded\n");
}

module_init(pmp5rep_init);
module_exit(pmp5rep_exit);
MODULE_LICENSE("GPL");
