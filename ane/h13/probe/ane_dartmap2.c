// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane_dartmap2.c — Gap12 window B (conditional GO): map the 22G74 fw VM
 * window on dart0 stream 0 for the unbound-dart variant boot.
 *
 * Correction over the Gap9 ane_dartmap: the fw gets its window base from
 * ASC_SCRATCH0/1 = fw_iova (h13 asserts fw_iova == 0x1f000000000,
 * EOS_TEXT_IOVA, text_phys 0x10000a54000), so the dart MUST translate
 * IOVA 0x1f000000000..0x1f0050c000 — not the low 32-bit alias.
 *
 * Mapping (h13 contract): IOVA 0x1f000000000 + 0x0000000..0x00d4000 ->
 * TEXT PA 0x10000a54000; +0x00d4000..+0x050c000 -> DATA PA 0x10001684000.
 *
 * Walk layout (io-pgtable-dart APPLE_DART2, 16K granule, u64 PTEs,
 * 2048 PTEs per 16K table): bits_per_level = 11, pgd entry covers 2^36.
 *   ias <= 36 -> 2-level (TTBR -> L1 -> leaf); the t6000 apple-dart driver
 *   hardcodes ias=32 and can therefore never reach the VM window — this
 *   module is the raw-table bypass of that SOFTWARE limit.
 *   The window top is 0x1f0050c000 < 2^37, so ANY mapping needs the
 *   3-level walk: pgd[iova>>36] -> mid[(iova>>25)&0x7ff] -> leaf.
 *   The hardware walk depth is the open question: PARAMS3 (0x08, never
 *   read before Gap12 window A) carries VA_WIDTH GENMASK(21,16) in the
 *   t8110 layout the 7.1.13 driver uses for t8110 only. REFUSE rule:
 *   PAGE_SHIFT (PARAMS1 GENMASK(27,24)) != 14 or VA_WIDTH < 37 -> NO
 *   WRITE, evidence logged (the designed park outcome; bypass / 32-bit
 *   alias fallbacks need an h13 fw_iova change and are NOT pre-registered).
 *
 * PTE encodings (io-pgtable-dart.c APPLE_DART2):
 *   leaf  = ((pa >> 4) & GENMASK(37,10)) | SP_END(0xfff << 40) | VALID(1)
 *         = ((pa >> 14) << 10) | (0xfffULL << 40) | 1   for granule-aligned pa
 *   table = ((pa >> 14) << 10) | 1
 *   TTBR  = DART_T8020_TTBR_VALID(0x200, BIT31) | pa >> 12
 *
 * Gate law (ane_dartraw precedent): pmgr ACTUAL nibble (bits 7:4) of the
 * owning power domains must read 0xf before any MMIO; otherwise refuse.
 * Log-then-write for every register write; readback verified; rollback =
 * reboot (pre-registered); tables kept alive until unload; on PASS the
 * module STAYS LOADED until the restore reboot. go=1 required.
 * One register group: dart0 stream-0 TTBR + the stream-0 TLB invalidate.
 */
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#define DART_T8020_STREAM_SELECT	0x34
#define DART_T8020_STREAM_COMMAND	0x20
#define DART_T8020_STREAM_COMMAND_BUSY	BIT(2)
#define DART_T8020_STREAM_COMMAND_INVALIDATE BIT(20)
#define DART_T8020_TTBR_VALID		BIT(31)
#define DART_T8020_TTBR			0x200
#define DART_T8020_TCR			0x100

/* never-read-before-Gap12 params (t8110 field layout, raw read) */
#define PARAMS1_PAGE_SHIFT(v)		(((v) >> 24) & 0xf)
#define PARAMS3_PA_WIDTH(v)		(((v) >> 24) & 0x3f)
#define PARAMS3_VA_WIDTH(v)		(((v) >> 16) & 0x3f)

#define TEXT_BASE	0x10000a54000ULL
#define TEXT_VMSIZE	0xd4000UL
#define DATA_BASE	0x10001684000ULL
#define DATA_VMSIZE	0x438000UL
#define PAGE_SZ		0x4000UL
#define PTES_PER_TBL	2048UL

#define VM_BASE		0x1f000000000ULL
#define VM_SPAN		(TEXT_VMSIZE + DATA_VMSIZE)	/* 0x50c000, < 2^37 */
#define PGD_IDX		(VM_BASE >> 36)
#define MID_IDX		((VM_BASE >> 25) & (PTES_PER_TBL - 1))

static int go;
module_param(go, int, 0444);
MODULE_PARM_DESC(go, "must be 1 to allow the dart0 TTBR write");

static int darts = 1;
module_param(darts, int, 0444);
MODULE_PARM_DESC(darts, "bitmask of darts to program TTBR0 on (default 1 = dart0)");

static unsigned long dart0_pa = 0x285800000UL;
module_param(dart0_pa, ulong, 0444);

/* pmgr pwrstate ACTUAL gates (same PAs as ane_dartraw) */
static unsigned long gate_sys = 0x28e080268UL;
static unsigned long gate_cpu = 0x28e0802c8UL;
static unsigned long gate_set0 = 0x28e08c000UL;

static u64 __iomem *pgd, *mid, *leaf;
static dma_addr_t pgd_dma, mid_dma, leaf_dma;
static void __iomem *dmap;
static struct platform_device *hold;

static u64 pte_leaf(u64 pa)
{
	return ((pa >> 14) << 10) | (0xfffULL << 40) | 1;
}
static u64 pte_table(u64 pa)
{
	return ((pa >> 14) << 10) | 1;
}

static int gate_check(const char *name, unsigned long pa)
{
	void __iomem *r = ioremap_np(pa, 4);
	u32 w;

	if (!r) {
		pr_err("dartmap2: gate %s ioremap failed\n", name);
		return -1;
	}
	w = readl(r);
	iounmap(r);
	pr_info("dartmap2: gate %s @%#lx = %#x (ACTUAL=%x)\n",
		name, pa, w, (w >> 4) & 0xf);
	return ((w >> 4) & 0xf) == 0xf ? 0 : -1;
}

static int __init dartmap2_init(void)
{
	unsigned long i;
	u32 old, new, rb, p1, p2, p3, p4, tcr0;
	int va_width, ret;

	if (!go) {
		pr_err("dartmap2: go=0, refusing (conditional-write guard)\n");
		return -EPERM;
	}
	if (gate_check("ane_sys", gate_sys) || gate_check("ane_sys_cpu", gate_cpu) ||
	    gate_check("ane_set0", gate_set0)) {
		pr_err("dartmap2: domain off — refusing dart MMIO\n");
		return -EIO;
	}

	dmap = ioremap_np(dart0_pa, 0x4000);
	if (!dmap)
		return -ENOMEM;

	/* full PARAMS receipt BEFORE any decision (first-ever PARAMS3/4 read) */
	p1 = readl(dmap + 0x00);
	p2 = readl(dmap + 0x04);
	p3 = readl(dmap + 0x08);
	p4 = readl(dmap + 0x0c);
	tcr0 = readl(dmap + DART_T8020_TCR);
	pr_info("dartmap2: PARAMS1=%#08x PARAMS2=%#08x PARAMS3=%#08x PARAMS4=%#08x TCR0=%#08x TTBR00=%#08x TTBR01=%#08x TTBR02=%#08x TTBR03=%#08x\n",
		p1, p2, p3, p4, tcr0,
		readl(dmap + 0x200), readl(dmap + 0x204),
		readl(dmap + 0x208), readl(dmap + 0x20c));

	ret = -EINVAL;
	if (PARAMS1_PAGE_SHIFT(p1) != 14) {
		pr_err("dartmap2: REFUSE granule: PAGE_SHIFT=%u != 14\n",
		       PARAMS1_PAGE_SHIFT(p1));
		goto out_unmap;
	}
	va_width = PARAMS3_VA_WIDTH(p3);
	pr_info("dartmap2: decode: PAGE_SHIFT=%u VA_WIDTH=%d PA_WIDTH=%u bypass=%d; VM window 0x%llx..0x%llx needs VA_WIDTH>=37\n",
		PARAMS1_PAGE_SHIFT(p1), va_width, PARAMS3_PA_WIDTH(p3),
		(int)(p2 & 1), VM_BASE, VM_BASE + VM_SPAN - 1);
	if (va_width < 37) {
		pr_err("dartmap2: REFUSE VM-WINDOW-UNMAPPABLE: VA_WIDTH=%d < 37 — no write; bypass/alias fallbacks need an h13 fw_iova change (not pre-registered)\n",
		       va_width);
		goto out_unmap;
	}

	hold = platform_device_register_simple("ane_dartmap2", -1, NULL, 0);
	if (IS_ERR(hold)) {
		ret = PTR_ERR(hold);
		goto out_unmap;
	}
	if (dma_set_mask_and_coherent(&hold->dev, DMA_BIT_MASK(42))) {
		platform_device_unregister(hold);
		hold = NULL;
		ret = -EIO;
		goto out_unmap;
	}

	pgd = dma_alloc_coherent(&hold->dev, PAGE_SZ, &pgd_dma, GFP_KERNEL);
	mid = dma_alloc_coherent(&hold->dev, PAGE_SZ, &mid_dma, GFP_KERNEL);
	leaf = dma_alloc_coherent(&hold->dev, PAGE_SZ, &leaf_dma, GFP_KERNEL);
	if (!pgd || !mid || !leaf)
		goto err_mem;

	pr_info("dartmap2: pgd @%pad mid @%pad leaf @%pad; pgd_idx=%llu mid_idx=%lu, %lu leaf pages\n",
		&pgd_dma, &mid_dma, &leaf_dma, PGD_IDX, MID_IDX, VM_SPAN / PAGE_SZ);
	for (i = 0; i < VM_SPAN / PAGE_SZ; i++) {
		u64 va = i * PAGE_SZ;
		u64 pa = va < TEXT_VMSIZE ? TEXT_BASE + va
					  : DATA_BASE + (va - TEXT_VMSIZE);
		leaf[i] = cpu_to_le64(pte_leaf(pa));
	}
	mid[MID_IDX] = cpu_to_le64(pte_table(leaf_dma));
	pgd[PGD_IDX] = cpu_to_le64(pte_table(mid_dma));
	dma_wmb();
	pr_info("dartmap2: pgd[%llu]=%016llx mid[%lu]=%016llx leaf[0]=%016llx leaf[%lu]=%016llx\n",
		PGD_IDX, le64_to_cpu(pgd[PGD_IDX]), MID_IDX,
		le64_to_cpu(mid[MID_IDX]), le64_to_cpu(leaf[0]),
		VM_SPAN / PAGE_SZ - 1, le64_to_cpu(leaf[VM_SPAN / PAGE_SZ - 1]));

	old = readl(dmap + DART_T8020_TTBR);
	new = DART_T8020_TTBR_VALID | ((u32)(pgd_dma >> 12));
	pr_info("dartmap2: dart0 TTBR0 @%#lx: old=%#08x -> new=%#08x (pgd pa=%pad)\n",
		dart0_pa + DART_T8020_TTBR, old, new, &pgd_dma);

	if (darts & 1) {
		pr_info("dartmap2: write next: dart0 +0x200 = %#08x\n", new);
		writel(new, dmap + DART_T8020_TTBR);
		rb = readl(dmap + DART_T8020_TTBR);
		pr_info("dartmap2: dart0 TTBR0 readback=%#08x %s\n", rb,
			rb == new ? "OK" : "MISMATCH");
		ret = 0;
	}

	/* stream-0 TLB invalidate so the walk sees the new TTBR */
	if (darts & 1) {
		u32 cmd;

		pr_info("dartmap2: write next: STREAM_SELECT = 1\n");
		writel(1, dmap + DART_T8020_STREAM_SELECT);
		pr_info("dartmap2: write next: STREAM_COMMAND = invalidate\n");
		writel(DART_T8020_STREAM_COMMAND_INVALIDATE,
		       dmap + DART_T8020_STREAM_COMMAND);
		ret = readl_poll_timeout(dmap + DART_T8020_STREAM_COMMAND,
					 cmd, !(cmd & DART_T8020_STREAM_COMMAND_BUSY),
					 1, 100);
		pr_info("dartmap2: invalidate busy-wait rc=%d cmd=%#x\n", ret, cmd);
	}
	pr_info("dartmap2: post TCR0=%#08x TTBR0=%#08x\n",
		readl(dmap + DART_T8020_TCR), readl(dmap + DART_T8020_TTBR));
	iounmap(dmap);
	return 0;

err_mem:
	if (leaf)
		dma_free_coherent(&hold->dev, PAGE_SZ, leaf, leaf_dma);
	if (mid)
		dma_free_coherent(&hold->dev, PAGE_SZ, mid, mid_dma);
	if (pgd)
		dma_free_coherent(&hold->dev, PAGE_SZ, pgd, pgd_dma);
	platform_device_unregister(hold);
	hold = NULL;
out_unmap:
	iounmap(dmap);
	return ret;
}
static void __exit dartmap2_exit(void)
{
	if (hold) {
		if (leaf)
			dma_free_coherent(&hold->dev, PAGE_SZ, leaf, leaf_dma);
		if (mid)
			dma_free_coherent(&hold->dev, PAGE_SZ, mid, mid_dma);
		if (pgd)
			dma_free_coherent(&hold->dev, PAGE_SZ, pgd, pgd_dma);
		platform_device_unregister(hold);
	}
	pr_info("dartmap2: unloaded, tables freed\n");
}
module_init(dartmap2_init);
module_exit(dartmap2_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Gap12 conditional dart0 stream-0 VM-window table for the 22G74 eos image");
