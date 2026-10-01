// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane_dartmap.c — Gap9 window B (conditional GO): give dart0 stream 0 a
 * valid translation table on the unbound-DT variant boot, replicating the
 * H167 working-M1 dart layout (translate on, shared valid table) for the
 * 22G74 eos image.
 *
 * Mapping (vm = low-32 IOVA, PA = carveout PAs; ADT segment-ranges analog,
 * Gap7 03:25Z decode): IOVA 0x0000000..0x00d4000 -> TEXT PA 0x10000a54000;
 * IOVA 0x00d4000..0x050c000 -> DATA PA 0x10001684000.
 *
 * Page-table format = kernel io-pgtable-dart APPLE_DART2 / t6000 hw:
 * 16K granule, 1 TTBR (ias 32), L1 table (2048 entries) whose entry 0 is a
 * table descriptor to the L2 leaf table (2048 x 16K = 32 MB).
 *   leaf PTE = ((pa >> 14) << 10) | SP_END(0xfff << 40) | VALID(1)
 *   table PTE = ((pa >> 14) << 10) | VALID(1)
 *   TTBR reg  = BIT(31) | (l1_pa >> 12)   (t8020 TTBR_VALID | pa >> 12)
 *
 * Safety: log-then-write for every register write; reads verify each write;
 * rollback = reboot (stated in the pre-registered plan); the module keeps
 * the table pages alive until unload, and on PASS it must stay loaded until
 * the restore reboot. go=1 required or the module refuses.
 * One register group this window: dart0 stream-0 TTBR (plus the stream-0
 * TLB invalidate that the write requires).
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

#define TEXT_BASE	0x10000a54000ULL
#define TEXT_VMSIZE	0xd4000UL
#define DATA_BASE	0x10001684000ULL
#define DATA_VMSIZE	0x438000UL
#define PAGE_SZ		0x4000UL
#define PTES		2048UL

static int go;
module_param(go, int, 0444);
MODULE_PARM_DESC(go, "must be 1 to allow the dart0 TTBR write");

static int darts = 1;
module_param(darts, int, 0444);
MODULE_PARM_DESC(darts, "bitmask of darts to program TTBR0 on (default 1 = dart0)");

static unsigned long dart0_pa = 0x285800000UL;
module_param(dart0_pa, ulong, 0444);

static u64 __iomem *l1, *l2;
static dma_addr_t l1_dma, l2_dma;
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

static int __init dartmap_init(void)
{
	unsigned long i, n;
	u32 old, new, rb;
	u64 va, pa;

	if (!go) {
		pr_err("dartmap: go=0, refusing (conditional-write guard)\n");
		return -EPERM;
	}

	hold = platform_device_register_simple("ane_dartmap", -1, NULL, 0);
	if (IS_ERR(hold))
		return PTR_ERR(hold);
	if (dma_set_mask_and_coherent(&hold->dev, DMA_BIT_MASK(42))) {
		platform_device_unregister(hold);
		return -EIO;
	}

	l1 = dma_alloc_coherent(&hold->dev, PAGE_SZ, &l1_dma, GFP_KERNEL);
	l2 = dma_alloc_coherent(&hold->dev, PAGE_SZ, &l2_dma, GFP_KERNEL);
	if (!l1 || !l2)
		goto err_mem;

	n = (TEXT_VMSIZE + DATA_VMSIZE) / PAGE_SZ;
	pr_info("dartmap: L1 @%pad L2 @%pad, %lu leaf pages\n",
		&l1_dma, &l2_dma, n);
	for (i = 0; i < n; i++) {
		va = i * PAGE_SZ;
		pa = va < TEXT_VMSIZE ? TEXT_BASE + va
				      : DATA_BASE + (va - TEXT_VMSIZE);
		l2[i] = cpu_to_le64(pte_leaf(pa));
	}
	l1[0] = cpu_to_le64(pte_table(l2_dma));
	dma_wmb();
	pr_info("dartmap: L1[0]=%016llx L2[0]=%016llx L2[52]=%016llx L2[53]=%016llx L2[%lu]=%016llx\n",
		le64_to_cpu(l1[0]), le64_to_cpu(l2[0]), le64_to_cpu(l2[52]),
		le64_to_cpu(l2[53]), n - 1, le64_to_cpu(l2[n - 1]));

	dmap = ioremap_np(dart0_pa, 0x4000);
	if (!dmap)
		goto err_mem;

	old = readl(dmap + DART_T8020_TTBR);
	new = DART_T8020_TTBR_VALID | ((u32)(l1_dma >> 12));
	pr_info("dartmap: dart0 TTBR0 @%#lx: old=%#08x -> new=%#08x (L1 pa=%pad)\n",
		dart0_pa + DART_T8020_TTBR, old, new, &l1_dma);

	if (darts & 1) {
		pr_info("dartmap: write next: dart0 +0x200 = %#08x\n", new);
		writel(new, dmap + DART_T8020_TTBR);
		rb = readl(dmap + DART_T8020_TTBR);
		pr_info("dartmap: dart0 TTBR0 readback=%#08x %s\n", rb,
			rb == new ? "OK" : "MISMATCH");
	}

	/* stream-0 TLB invalidate so the walk sees the new TTBR */
	if (darts & 1) {
		u32 cmd;
		int ret;

		pr_info("dartmap: write next: STREAM_SELECT = 1\n");
		writel(1, dmap + DART_T8020_STREAM_SELECT);
		pr_info("dartmap: write next: STREAM_COMMAND = invalidate\n");
		writel(DART_T8020_STREAM_COMMAND_INVALIDATE,
		       dmap + DART_T8020_STREAM_COMMAND);
		ret = readl_poll_timeout(dmap + DART_T8020_STREAM_COMMAND,
					 cmd, !(cmd & DART_T8020_STREAM_COMMAND_BUSY),
					 1, 100);
		pr_info("dartmap: invalidate busy-wait rc=%d cmd=%#x\n",
			ret, cmd);
	}
	pr_info("dartmap: post TCR0=%#08x TTBR0=%#08x\n",
		readl(dmap + DART_T8020_TCR),
		readl(dmap + DART_T8020_TTBR));
	iounmap(dmap);
	return 0;

err_mem:
	if (l1)
		dma_free_coherent(&hold->dev, PAGE_SZ, l1, l1_dma);
	if (l2)
		dma_free_coherent(&hold->dev, PAGE_SZ, l2, l2_dma);
	platform_device_unregister(hold);
	return -ENOMEM;
}
static void __exit dartmap_exit(void)
{
	if (dmap)
		iounmap(dmap);
	if (l1)
		dma_free_coherent(&hold->dev, PAGE_SZ, l1, l1_dma);
	if (l2)
		dma_free_coherent(&hold->dev, PAGE_SZ, l2, l2_dma);
	platform_device_unregister(hold);
	pr_info("dartmap: unloaded, tables freed\n");
}
module_init(dartmap_init);
module_exit(dartmap_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Gap9 conditional dart0 stream-0 table for the 22G74 eos image");
