// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Joshua Warren */

#include <linux/interrupt.h>
#include <linux/gfp.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/iopoll.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "ane.h"

/*
 * T8103 DART is the T8020 register block (apple_dart_hw_t8103 in
 * drivers/iommu/apple-dart.c), pagesized 0x4000. Containment policy:
 * during a job the apple-dart fault IRQ is masked so the latch survives
 * for this driver to read, the completion poll checks that latch before
 * every engine access (engine reads hang the SoC once a fault has
 * landed), and a faulted IOVA is handed a throwaway page so the retried
 * transaction completes and the engine drains. Mapping itself stays
 * provider-owned; the page tables are never rewritten here.
 */
#define DART_STREAM_COMMAND		0x20
#define DART_STREAM_COMMAND_BUSY	BIT(2)
#define DART_STREAM_COMMAND_INVALIDATE	BIT(20)
#define DART_STREAM_SELECT		0x34
#define DART_ERROR			0x40
#define DART_ERROR_FLAG			BIT(31)
#define DART_ERROR_ADDR_LO		0x50
#define DART_ERROR_ADDR_HI		0x54
#define DART_STREAMS_ENABLE		0xfc
#define DART_TCR			0x100
#define DART_TTBR			0x200
#define DART_TTBR_COUNT			4

#define DART_TCR_SID(sid) (DART_TCR + ((sid) << 2))
#define DART_TTBR_SID(sid, idx) \
	(DART_TTBR + ((((sid) * DART_TTBR_COUNT) + (idx)) << 2))

static bool dart_contain = true;
module_param(dart_contain, bool, 0444);
MODULE_PARM_DESC(dart_contain,
		 "Mask ANE DART fault IRQs during a job and restore the stream after a fault (default: on)");

static int ane_dart_invalidate(struct ane_device *ane, struct ane_dart *dart)
{
	void __iomem *regs = dart->regs;
	u32 cmd;
	int ret;

	if (dart->sid >= 32)
		return -EINVAL;
	writel(BIT(dart->sid), regs + DART_STREAM_SELECT);
	writel(DART_STREAM_COMMAND_INVALIDATE, regs + DART_STREAM_COMMAND);
	ret = readl_poll_timeout(regs + DART_STREAM_COMMAND, cmd,
				 !(cmd & DART_STREAM_COMMAND_BUSY), 1, 100);
	if (ret)
		dev_err(ane->dev, "DART sid %u TLB invalidate timed out\n",
			dart->sid);
	return ret;
}

int ane_dart_init(struct ane_device *ane)
{
	struct device_node *np = ane->dev->of_node;
	int count, i;

	ane->dart_count = 0;
	if (!dart_contain)
		return 0;
	count = of_count_phandle_with_args(np, "iommus", "#iommu-cells");
	if (count < 0)
		return 0;
	for (i = 0; i < count && ane->dart_count < ANE_DART_MAX; i++) {
		struct of_phandle_args args;
		struct platform_device *pdev;
		struct resource *res;
		struct ane_dart *dart;
		void __iomem *regs;
		int irq;

		if (of_parse_phandle_with_args(np, "iommus", "#iommu-cells",
					       i, &args))
			break;
		if (!of_device_is_compatible(args.np, "apple,t8103-dart")) {
			of_node_put(args.np);
			continue;
		}
		pdev = of_find_device_by_node(args.np);
		if (!pdev) {
			of_node_put(args.np);
			continue;
		}
		res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		irq = platform_get_irq(pdev, 0);
		if (!res || irq < 0) {
			put_device(&pdev->dev);
			of_node_put(args.np);
			continue;
		}
		regs = devm_ioremap(ane->dev, res->start, resource_size(res));
		if (!regs) {
			put_device(&pdev->dev);
			of_node_put(args.np);
			return -ENOMEM;
		}
		dart = &ane->darts[ane->dart_count++];
		dart->regs = regs;
		dart->irq = irq;
		dart->sid = args.args_count ? args.args[0] : 0;
		dart->masked = false;
		dev_info(ane->dev, "DART containment: %pOFn sid %u irq %d\n",
			 args.np, dart->sid, irq);
		put_device(&pdev->dev);
		of_node_put(args.np);
	}
	dev_info(ane->dev, "DART containment armed: %d\n", ane->dart_count);
	return 0;
}

void ane_dart_mask(struct ane_device *ane)
{
	int i;

	for (i = 0; i < ane->dart_count; i++) {
		struct ane_dart *dart = &ane->darts[i];

		if (dart->masked)
			continue;
		disable_irq(dart->irq);
		dart->masked = true;
	}
}

void ane_dart_unmask(struct ane_device *ane)
{
	int i;

	for (i = 0; i < ane->dart_count; i++) {
		struct ane_dart *dart = &ane->darts[i];

		if (!dart->masked)
			continue;
		enable_irq(dart->irq);
		dart->masked = false;
	}
}

bool ane_dart_faulted(struct ane_device *ane, u64 *iova, u32 *status)
{
	int i;

	for (i = 0; i < ane->dart_count; i++) {
		void __iomem *regs = ane->darts[i].regs;
		u32 err = readl(regs + DART_ERROR);

		if (!(err & DART_ERROR_FLAG))
			continue;
		if (status)
			*status = err;
		if (iova)
			*iova = readl(regs + DART_ERROR_ADDR_LO) |
				((u64)readl(regs + DART_ERROR_ADDR_HI) << 32);
		return true;
	}
	return false;
}

/*
 * A fault is a missing PTE, nothing more: the page tables are provider
 * owned and unchanged, so the stream-restore apple-dart uses at reset
 * time is the wrong tool mid-job (register surgery while the faulting
 * transaction is retried is what killed jwm1 twice). Instead the faulted
 * IOVA is handed a throwaway DART page, so the retried transaction
 * completes and the engine drains on its own; the scratch pages are
 * unmapped once the request has been failed cleanly.
 *
 * The throwaway page is mapped at the faulting address, which is
 * allocator-free by definition of a translation fault - but the drm_mm
 * allocator knows nothing about a raw iommu_map. A scratch PTE that
 * outlives its release path (any future bug on the wedge side) would
 * then sit, untracked, in a range the allocator considers free: the
 * next BO_INIT maps onto it and dies in dart_init_pte's "we require an
 * unmap first" WARN (m1-test-host 2026-09-25, iommu_map failed at 0x4000). So
 * the scratch range is RESERVED in the allocator first: a node-tracked
 * mapping can never collide with a later BO_INIT, and a scratch whose
 * release is skipped degrades into a bounded, benign leak instead of a
 * poisoned address space. Reserve failure means a live BO node already
 * owns the faulting page (a wild descriptor, not a hole): refuse and
 * let the caller wedge, fail closed.
 */
int ane_dart_drain_fault(struct ane_device *ane, u64 fault_iova,
			 struct ane_dart_scratch *scratch, int *pages)
{
	struct drm_mm_node *node;
	struct page *page;
	u64 iova;
	int ret, i;

	page = alloc_page(GFP_KERNEL);
	if (!page)
		return -ENOMEM;
	iova = fault_iova & ~((1ULL << ane->shift) - 1);

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node) {
		__free_page(page);
		return -ENOMEM;
	}
	node->start = iova;
	node->size = BIT(ane->shift);

	mutex_lock(&ane->iommu_lock);
	ret = drm_mm_reserve_node(&ane->mm, node);
	if (ret) {
		mutex_unlock(&ane->iommu_lock);
		kfree(node);
		__free_page(page);
		return ret;
	}

	ret = iommu_map(ane->domain, iova, page_to_phys(page),
			BIT(ane->shift), IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
	mutex_unlock(&ane->iommu_lock);
	if (ret) {
		mutex_lock(&ane->iommu_lock);
		drm_mm_remove_node(node);
		mutex_unlock(&ane->iommu_lock);
		kfree(node);
		__free_page(page);
		return ret;
	}
	scratch[*pages].iova = iova;
	scratch[*pages].page = page;
	scratch[*pages].node = node;
	(*pages)++;
	for (i = 0; i < ane->dart_count; i++)
		ane_dart_invalidate(ane, &ane->darts[i]);
	return 0;
}

void ane_dart_release_scratch(struct ane_device *ane,
			      struct ane_dart_scratch *scratch, int pages)
{
	int i, d;

	mutex_lock(&ane->iommu_lock);
	for (i = 0; i < pages; i++) {
		size_t unmapped;

		unmapped = iommu_unmap(ane->domain, scratch[i].iova,
				       BIT(ane->shift));
		if (unmapped != BIT(ane->shift))
			dev_err(ane->dev,
				"scratch unmap short at %#llx: %zu\n",
				scratch[i].iova, unmapped);
		drm_mm_remove_node(scratch[i].node);
		kfree(scratch[i].node);
		scratch[i].node = NULL;
		__free_page(scratch[i].page);
		scratch[i].page = NULL;
	}
	mutex_unlock(&ane->iommu_lock);
	if (pages)
		for (d = 0; d < ane->dart_count; d++)
			ane_dart_invalidate(ane, &ane->darts[d]);
}
