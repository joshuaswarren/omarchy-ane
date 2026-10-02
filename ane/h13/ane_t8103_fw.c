// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane_t8103_fw.c — T8103 ANE firmware start, stages 1-2 only (design:
 * docs/plans/2026-10-03-t8103-fw-start-port.md).
 *
 *   stage 1  read-only: power the ANE through genpd, re-read the ASC state
 *            (RVBAR, CPU_STATUS, SCRATCH, mailbox, doorbell) and verify that
 *            the iBoot-preloaded firmware TEXT in DRAM matches the pinned
 *            22G74 H13 payload.
 *   stage 2  stage 1, then build the firmware DATA segment from the payload
 *            and map it into the shared ANE DART domain at IOVA 0xbc000.
 *            No device register is written; the only state changed is the
 *            Linux-owned IOMMU page table, undone at rmmod.
 *
 * Stage 3 (CPU release) is deliberately not implemented here. Load with the
 * legacy `ane` module removed (`rmmod ane`); `modprobe ane` restores it after
 * rmmod of this module, no reboot needed at stages 1-2.
 *
 * Rules kept from H164: every ANE/pmgr access is ioremap_np (a posted mapping
 * reset the SoC three times), and the first access waits settle_ms after the
 * power-up.
 */
#include <crypto/sha2.h>
#include <linux/delay.h>
#include <linux/dma-map-ops.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/vmalloc.h>
#include <linux/debugfs.h>

#include "../src/ane_ps.h"

#define ENGINE_PA	0x26a000000ULL
#define ENGINE_SZ	0x2000000ULL
#define PS_PA		0x23b70c000ULL
#define R_RVBAR		0x1050000
#define R_CPU_STATUS	0x1400048
#define R_MBOX_A2I	0x1408110
#define R_MBOX_I2A	0x1408114
#define R_SCRATCH0	0x1840048
#define R_DOORBELL	0x1844000
#define R_DB_PENDING	0x184c000
#define R_DB_ACK	0x1850000

#define FW_SIZE		0x4bd4f0UL
#define FW_TEXT_FOFF	0x4000UL
#define FW_TEXT_SZ	0xbc000UL
#define FW_DATA_FOFF	0xc0000UL
#define FW_DATA_FSZ	0x3e8000UL
#define FW_DATA_VMSZ	0x438000UL
#define FW_DATA_IOVA	0xbc000UL
/* iBoot patches these two TEXT words (H163b): offsets inside TEXT. */
#define TEXT_PATCH0	0x423cUL
#define TEXT_PATCH1	0x4240UL

static const u8 fw_sha256[32] = {
	0x7f, 0x90, 0x6d, 0x11, 0x89, 0x7c, 0xb9, 0x30, 0xe5, 0xc8, 0xbf, 0xc1,
	0x55, 0xb5, 0x0c, 0x44, 0xd8, 0x03, 0xf8, 0xa6, 0x4f, 0xcf, 0x9e, 0xe9,
	0x91, 0x52, 0x10, 0x9f, 0x3a, 0x30, 0x4f, 0x34,
};

static unsigned int stage = 1;
module_param(stage, uint, 0444);
MODULE_PARM_DESC(stage, "1 = read-only checks, 2 = also stage the DATA segment in the DART, 3 = also release the ASC CPU (needs go=1)");
static bool go;
module_param(go, bool, 0444);
MODULE_PARM_DESC(go, "arm stage 3 (device writes: SCRATCH clear, CPU_CONTROL 0 then 0x10); a reboot or ANE power cycle undoes it");
static char *fw_name = "apple/ane/h13_ane_fw_13.5_22G74.bin";
module_param(fw_name, charp, 0444);
static unsigned long long text_phys = 0x800938000ULL;
module_param(text_phys, ullong, 0444);
MODULE_PARM_DESC(text_phys, "PA of the iBoot-preloaded TEXT (RVBAR entry, H166)");
static unsigned int settle_ms = 3000;
/* H177: the fw bootstrap zeroes then fills its page tables at DATA offsets 0x1c000..0x24000 (emulated reset head). Pre-filling
 * that range with 0xa5 turns "did the fw reach its first DATA write" into a visible change. */
#define PT_OFF 0x1c000UL
#define PT_LEN 0x8000UL
static bool fill_pt;
module_param(fill_pt, bool, 0444);
MODULE_PARM_DESC(fill_pt, "stage 2+: fill DATA 0x1c000..0x24000 with 0xa5 so a fw write there is visible");
module_param(settle_ms, uint, 0444);
/* H195: map the iBoot-STAGED TEXT/DATA (found by the userspace scan, H178) instead of a copy of the payload DATA. */
static bool staged;
module_param(staged, bool, 0444);
MODULE_PARM_DESC(staged, "stage 2+: map the iBoot-staged TEXT (text_phys) and DATA (data_phys) in the DART instead of a payload copy (H195)");
static unsigned long long data_phys;
module_param(data_phys, ullong, 0444);
MODULE_PARM_DESC(data_phys, "PA of the iBoot-staged DATA segment (from the H178 scan; moves per boot)");
static bool dry;
module_param(dry, bool, 0444);
MODULE_PARM_DESC(dry, "staged: verify and report only, map nothing");
static bool map_text = true;
module_param(map_text, bool, 0444);
MODULE_PARM_DESC(map_text, "staged: also map the staged TEXT at IOVA 0");

struct fw_ctx {
	struct device *dev;
	struct device **pd_dev;
	struct device_link **pd_link;
	int pd_count;
	void __iomem *eng;
	struct iommu_domain *dom;
	struct page **pages;
	unsigned long npages, mapped;
	unsigned long st_text_mapped, st_data_mapped;
	u8 *pre_text, *pre_data;
	bool pm_held;
	void *snap;
	struct debugfs_blob_wrapper blob;
	struct dentry *dbg;
};

static u32 rd(struct fw_ctx *c, u32 off)
{
	return readl(c->eng + off);
}

static void fw_detach_genpd(struct fw_ctx *c)
{
	for (int i = c->pd_count - 1; i >= 0 && c->pd_count > 1; i--) {
		if (c->pd_link[i])
			device_link_del(c->pd_link[i]);
		if (!IS_ERR_OR_NULL(c->pd_dev[i]))
			dev_pm_domain_detach(c->pd_dev[i], true);
	}
}

static int fw_attach_genpd(struct fw_ctx *c)
{
	struct device *dev = c->dev;

	c->pd_count = of_count_phandle_with_args(dev->of_node, "power-domains", "#power-domain-cells");
	if (c->pd_count < 1)
		return c->pd_count < 0 ? c->pd_count : -EINVAL;
	if (c->pd_count == 1)
		return dev->pm_domain ? 0 : -EPROBE_DEFER;
	c->pd_dev = devm_kcalloc(dev, c->pd_count, sizeof(*c->pd_dev), GFP_KERNEL);
	c->pd_link = devm_kcalloc(dev, c->pd_count, sizeof(*c->pd_link), GFP_KERNEL);
	if (!c->pd_dev || !c->pd_link)
		return -ENOMEM;
	for (int i = 0; i < c->pd_count; i++) {
		c->pd_dev[i] = dev_pm_domain_attach_by_id(dev, i);
		if (IS_ERR(c->pd_dev[i])) {
			int err = PTR_ERR(c->pd_dev[i]);

			c->pd_dev[i] = NULL;
			c->pd_count = i;
			fw_detach_genpd(c);
			return err;
		}
		c->pd_link[i] = device_link_add(dev, c->pd_dev[i], DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME | DL_FLAG_RPM_ACTIVE);
		if (!c->pd_link[i]) {
			c->pd_count = i + 1;
			fw_detach_genpd(c);
			return -EINVAL;
		}
	}
	return 0;
}

/* Stage 1: iBoot-preloaded TEXT vs the pinned payload. */
static int fw_check_text(struct fw_ctx *c, const struct firmware *fw)
{
	const u8 *want = fw->data + FW_TEXT_FOFF;
	unsigned long pg = PAGE_SIZE, ok = 0, bad = 0, off;
	void *mem = memremap(text_phys, FW_TEXT_SZ, MEMREMAP_WB);

	if (!mem)
		return -ENOMEM;
	for (off = 0; off < FW_TEXT_SZ; off += pg) {
		if (!memcmp(mem + off, want + off, pg)) {
			ok++;
			continue;
		}
		if (off <= TEXT_PATCH0 && TEXT_PATCH1 < off + pg) {
			/* only the two iBoot-patched words may differ */
			unsigned long i, n = 0;

			for (i = 0; i < pg; i++)
				if (((u8 *)mem)[off + i] != want[off + i] &&
				    (off + i < TEXT_PATCH0 || off + i >= TEXT_PATCH0 + 4) &&
				    (off + i < TEXT_PATCH1 || off + i >= TEXT_PATCH1 + 4))
					n++;
			if (!n) {
				ok++;
				dev_info(c->dev, "fw-start: TEXT page %lu differs only in the iBoot words (+%#lx=%#x, +%#lx=%#x)\n",
					 off / pg, TEXT_PATCH0, *(u32 *)(mem + TEXT_PATCH0), TEXT_PATCH1,
					 *(u32 *)(mem + TEXT_PATCH1));
				continue;
			}
		}
		bad++;
	}
	memunmap(mem);
	dev_info(c->dev, "fw-start: TEXT at %#llx: %lu/%lu pages match the payload, %lu bad\n", text_phys, ok,
		 FW_TEXT_SZ / pg, bad);
	return bad ? -EILSEQ : 0;
}

static void fw_unstage_data(struct fw_ctx *c)
{
	for (unsigned long i = 0; i < c->npages && c->pages; i++) {
		if (i < c->mapped)
			iommu_unmap(c->dom, FW_DATA_IOVA + i * PAGE_SIZE, PAGE_SIZE);
		if (c->pages[i])
			__free_page(c->pages[i]);
	}
	kvfree(c->pages);
	c->pages = NULL;
	c->mapped = c->npages = 0;
}

/* Stage 2: DATA from the payload, mapped at IOVA 0xbc000 in the shared domain. */
static int fw_stage_data(struct fw_ctx *c, const struct firmware *fw)
{
	int prot = IOMMU_READ | IOMMU_WRITE;
	unsigned long i, filled = 0;
	void *va;
	int err = 0;

	c->dom = iommu_get_domain_for_dev(c->dev);
	if (!c->dom)
		return -EPROBE_DEFER;
	if (FW_DATA_IOVA + FW_DATA_VMSZ - 1 > c->dom->geometry.aperture_end)
		return -ERANGE;
	if (dev_is_dma_coherent(c->dev))
		prot |= IOMMU_CACHE;
	c->npages = FW_DATA_VMSZ / PAGE_SIZE;
	c->pages = kvcalloc(c->npages, sizeof(*c->pages), GFP_KERNEL);
	if (!c->pages)
		return -ENOMEM;
	for (i = 0; i < c->npages; i++) {
		c->pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!c->pages[i]) {
			err = -ENOMEM;
			goto out;
		}
		if (iommu_iova_to_phys(c->dom, FW_DATA_IOVA + i * PAGE_SIZE)) {
			dev_err(c->dev, "fw-start: IOVA %#lx already mapped, refusing\n", FW_DATA_IOVA + i * PAGE_SIZE);
			err = -EEXIST;
			goto out;
		}
	}
	va = vmap(c->pages, c->npages, VM_MAP, PAGE_KERNEL);
	if (!va) {
		err = -ENOMEM;
		goto out;
	}
	memcpy(va, fw->data + FW_DATA_FOFF, FW_DATA_FSZ);
	if (fill_pt)
		memset(va + PT_OFF, 0xa5, PT_LEN);
	for (i = 0; i < c->npages; i++) {
		err = iommu_map(c->dom, FW_DATA_IOVA + i * PAGE_SIZE, page_to_phys(c->pages[i]), PAGE_SIZE, prot,
				GFP_KERNEL);
		if (err)
			break;
		c->mapped++;
	}
	dma_wmb();
	if (!err) {
		for (i = 0; i < c->npages; i++)
			if (iommu_iova_to_phys(c->dom, FW_DATA_IOVA + i * PAGE_SIZE) != page_to_phys(c->pages[i]))
				err = -EFAULT;
		filled = (fill_pt ? memcmp(va, fw->data + FW_DATA_FOFF, PT_OFF) : memcmp(va, fw->data + FW_DATA_FOFF, FW_DATA_FSZ)) ? 0 : FW_DATA_FSZ;
	}
	vunmap(va);
	dev_info(c->dev, "fw-start: DATA staged: %lu pages mapped at IOVA %#lx.., payload bytes verified %#lx, err %d\n",
		 c->mapped, FW_DATA_IOVA, filled, err);
out:
	if (err)
		fw_unstage_data(c);
	return err;
}

/* H195 ---------------------------------------------------------------------------------------------------------- */
static int fw_region_ok(struct fw_ctx *c, const char *what, u64 pa, size_t len)
{
	int r = region_intersects(pa, len, IORESOURCE_MEM, IORES_DESC_NONE);

	dev_info(c->dev, "fw-start: %s [%#llx, %#llx) region_intersects(IORESOURCE_MEM) = %d (REGION_DISJOINT = %d)\n",
		 what, pa, pa + len, r, REGION_DISJOINT);
	return r == REGION_DISJOINT ? 0 : -EBUSY;
}

static void fw_unstage_staged(struct fw_ctx *c)
{
	for (unsigned long i = 0; i < c->st_data_mapped; i++)
		iommu_unmap(c->dom, FW_DATA_IOVA + i * PAGE_SIZE, PAGE_SIZE);
	for (unsigned long i = 0; i < c->st_text_mapped; i++)
		iommu_unmap(c->dom, i * PAGE_SIZE, PAGE_SIZE);
	c->st_data_mapped = c->st_text_mapped = 0;
	kvfree(c->pre_data);
	kvfree(c->pre_text);
	c->pre_data = c->pre_text = NULL;
}

/* Verify the staged DATA against the payload (read-only), hash it twice, and unless dry map TEXT and DATA. */
static int fw_stage_staged(struct fw_ctx *c, const struct firmware *fw)
{
	int prot = IOMMU_READ | IOMMU_WRITE;
	unsigned long i, npg = FW_DATA_VMSZ / PAGE_SIZE, eqnz = 0, eqz = 0, diff = 0;
	u8 h1[32], h2[32];
	u8 *va, *tx;
	int err;

	if (!data_phys || (data_phys & (PAGE_SIZE - 1))) {
		dev_err(c->dev, "fw-start: staged needs a page-aligned data_phys\n");
		return -EINVAL;
	}
	err = fw_region_ok(c, "TEXT", text_phys, FW_TEXT_SZ);
	if (err)
		return err;
	err = fw_region_ok(c, "DATA", data_phys, FW_DATA_VMSZ);
	if (err)
		return err;
	va = memremap(data_phys, FW_DATA_VMSZ, MEMREMAP_WB);
	if (!va)
		return -ENOMEM;
	for (i = 0; i < npg; i++) {
		const u8 *p = va + i * PAGE_SIZE;
		const u8 *w = i * PAGE_SIZE < FW_DATA_FSZ ? fw->data + FW_DATA_FOFF + i * PAGE_SIZE : NULL;
		bool zero = !memchr_inv(p, 0, PAGE_SIZE);

		if (w ? !memcmp(p, w, PAGE_SIZE) : zero) {
			if (zero)
				eqz++;
			else
				eqnz++;
			continue;
		}
		diff++;
		if (diff <= 16) {
			unsigned long j, n = 0;

			for (j = 0; j < PAGE_SIZE; j++)
				if (p[j] != (w ? w[j] : 0))
					n++;
			dev_info(c->dev, "fw-start: staged DATA page %lu (+%#lx) differs from the payload in %lu bytes\n", i,
				 i * PAGE_SIZE, n);
		}
	}
	sha256(va, FW_DATA_FSZ, h1);
	memunmap(va);
	va = memremap(data_phys, FW_DATA_VMSZ, MEMREMAP_WB);
	if (!va)
		return -ENOMEM;
	sha256(va, FW_DATA_FSZ, h2);
	memunmap(va);
	dev_info(c->dev, "fw-start: staged DATA at %#llx: pages equal-nonzero %lu, equal-zero %lu, differing %lu of %lu; sha256 read1 %*phN read2 %*phN (%s)\n",
		 data_phys, eqnz, eqz, diff, npg, 8, h1, 8, h2, memcmp(h1, h2, 32) ? "UNSTABLE" : "stable");
	if (memcmp(h1, h2, 32) || eqnz < 6 || diff > 4) {
		dev_err(c->dev, "fw-start: staged DATA does not look like the payload (stable=%d eqnz=%lu diff=%lu): refusing\n",
			!memcmp(h1, h2, 32), eqnz, diff);
		return -EILSEQ;
	}
	tx = memremap(text_phys, FW_TEXT_SZ, MEMREMAP_WB);
	if (!tx)
		return -ENOMEM;
	dev_info(c->dev, "fw-start: staged TEXT at %#llx: word +%#lx = %#x, +%#lx = %#x\n", text_phys, TEXT_PATCH0,
		 *(u32 *)(tx + TEXT_PATCH0), TEXT_PATCH1, *(u32 *)(tx + TEXT_PATCH1));
	memunmap(tx);
	if (dry) {
		dev_info(c->dev, "fw-start: dry run, nothing mapped\n");
		return 0;
	}

	c->dom = iommu_get_domain_for_dev(c->dev);
	if (!c->dom)
		return -EPROBE_DEFER;
	if (FW_DATA_IOVA + FW_DATA_VMSZ - 1 > c->dom->geometry.aperture_end)
		return -ERANGE;
	if (dev_is_dma_coherent(c->dev))
		prot |= IOMMU_CACHE;
	for (i = 0; i < npg; i++)
		if (iommu_iova_to_phys(c->dom, FW_DATA_IOVA + i * PAGE_SIZE)) {
			dev_err(c->dev, "fw-start: IOVA %#lx already mapped, refusing\n", FW_DATA_IOVA + i * PAGE_SIZE);
			return -EEXIST;
		}
	if (map_text)
		for (i = 0; i < FW_TEXT_SZ / PAGE_SIZE; i++)
			if (iommu_iova_to_phys(c->dom, i * PAGE_SIZE)) {
				dev_err(c->dev, "fw-start: TEXT IOVA %#lx already mapped, refusing\n", i * PAGE_SIZE);
				return -EEXIST;
			}
	/* pre-RUN snapshots of the staged regions, for the post-RUN diff */
	c->pre_data = kvmalloc(FW_DATA_VMSZ, GFP_KERNEL);
	c->pre_text = kvmalloc(FW_TEXT_SZ, GFP_KERNEL);
	if (!c->pre_data || !c->pre_text) {
		err = -ENOMEM;
		goto fail;
	}
	va = memremap(data_phys, FW_DATA_VMSZ, MEMREMAP_WB);
	tx = memremap(text_phys, FW_TEXT_SZ, MEMREMAP_WB);
	if (!va || !tx) {
		if (va)
			memunmap(va);
		if (tx)
			memunmap(tx);
		err = -ENOMEM;
		goto fail;
	}
	memcpy(c->pre_data, va, FW_DATA_VMSZ);
	memcpy(c->pre_text, tx, FW_TEXT_SZ);
	memunmap(va);
	memunmap(tx);
	for (i = 0; i < npg; i++) {
		err = iommu_map(c->dom, FW_DATA_IOVA + i * PAGE_SIZE, data_phys + i * PAGE_SIZE, PAGE_SIZE, prot, GFP_KERNEL);
		if (err)
			goto fail;
		c->st_data_mapped++;
	}
	if (map_text)
		for (i = 0; i < FW_TEXT_SZ / PAGE_SIZE; i++) {
			err = iommu_map(c->dom, i * PAGE_SIZE, text_phys + i * PAGE_SIZE, PAGE_SIZE, prot, GFP_KERNEL);
			if (err)
				goto fail;
			c->st_text_mapped++;
		}
	dma_wmb();
	for (i = 0; i < c->st_data_mapped; i++)
		if (iommu_iova_to_phys(c->dom, FW_DATA_IOVA + i * PAGE_SIZE) != data_phys + i * PAGE_SIZE)
			err = -EFAULT;
	for (i = 0; i < c->st_text_mapped; i++)
		if (iommu_iova_to_phys(c->dom, i * PAGE_SIZE) != text_phys + i * PAGE_SIZE)
			err = -EFAULT;
	dev_info(c->dev, "fw-start: staged mapped: DATA %lu pages IOVA %#lx -> PA %#llx, TEXT %lu pages IOVA 0 -> PA %#llx, prot %#x, err %d\n",
		 c->st_data_mapped, FW_DATA_IOVA, data_phys, c->st_text_mapped, text_phys, prot, err);
	if (!err)
		return 0;
fail:
	fw_unstage_staged(c);
	return err;
}

/* Post-RUN diff of the staged regions against the pre-RUN snapshots (read-only). */
static void fw_readback_staged(struct fw_ctx *c, const char *label)
{
	u8 *d = memremap(data_phys, FW_DATA_VMSZ, MEMREMAP_WB);
	u8 *t = memremap(text_phys, FW_TEXT_SZ, MEMREMAP_WB);
	unsigned long i, ch_d = 0, ch_t = 0;

	if (!d || !t || !c->pre_data || !c->pre_text) {
		dev_err(c->dev, "fw-start: %s staged readback unavailable\n", label);
		goto out;
	}
	for (i = 0; i < FW_DATA_VMSZ; i += PAGE_SIZE) {
		if (!memcmp(d + i, c->pre_data + i, PAGE_SIZE))
			continue;
		ch_d++;
		if (ch_d <= 24) {
			unsigned long j, n = 0, first = 0;

			for (j = 0; j < PAGE_SIZE; j++)
				if (d[i + j] != c->pre_data[i + j]) {
					if (!n)
						first = j;
					n++;
				}
			dev_info(c->dev, "fw-start: %s staged DATA page %lu (+%#lx) written: %lu bytes, first at +%#lx (%#x -> %#x)\n",
				 label, i / PAGE_SIZE, i, n, i + first, c->pre_data[i + first], d[i + first]);
		}
	}
	for (i = 0; i < FW_TEXT_SZ; i += PAGE_SIZE)
		if (memcmp(t + i, c->pre_text + i, PAGE_SIZE)) {
			ch_t++;
			if (ch_t <= 8)
				dev_info(c->dev, "fw-start: %s staged TEXT page %lu (+%#lx) written\n", label, i / PAGE_SIZE, i);
		}
	dev_info(c->dev, "fw-start: %s staged DATA: %lu of %lu pages written since pre-RUN; staged TEXT: %lu of %lu pages\n",
		 label, ch_d, FW_DATA_VMSZ / PAGE_SIZE, ch_t, FW_TEXT_SZ / PAGE_SIZE);
out:
	if (d)
		memunmap(d);
	if (t)
		memunmap(t);
}

/* H175: the three ANE DARTs' error latch (ERROR, ADDR lo/hi) and the per-stream TCR words, non-posted reads. The standard
 * apple-dart T8020 offsets (0x40/0x50/0x54, TCR 0x100 + 4*sid) are an assumption; H167 read nonzero latched values at rest. */
static void fw_dart_dump(struct fw_ctx *c, const char *label)
{
	for (int n = 0; n < 3; n++) {
		void __iomem *d = ioremap_np(0x26b800000ULL + n * 0x10000ULL, 0x4000);
		u32 tcr[16];

		if (!d)
			continue;
		for (int s = 0; s < 16; s++)
			tcr[s] = readl(d + 0x100 + 4 * s);
		dev_info(c->dev, "fw-start: %s DART%d err %#x addr %#x_%08x TCR0-15 %#x %#x %#x %#x %#x %#x %#x %#x %#x %#x %#x %#x %#x %#x %#x %#x\n",
			 label, n, readl(d + 0x40), readl(d + 0x54), readl(d + 0x50), tcr[0], tcr[1], tcr[2], tcr[3], tcr[4],
			 tcr[5], tcr[6], tcr[7], tcr[8], tcr[9], tcr[10], tcr[11], tcr[12], tcr[13], tcr[14], tcr[15]);
		iounmap(d);
	}
}

/* BANNED (H176 incident 2026-10-01): reading the ASC CoreSight debug block (engine+0x1010000.., EDSCR/EDPCSR) from the AP
 * hard-reset jwm1. Do not add AP reads of that block back; see the omarchy-mplus-private receipt 2026-10-01-jwm1-h176-incident. */
/* H174: read the staged DATA back through the CPU mapping of the same pages the DART maps and report what the fw wrote. */
static void fw_readback(struct fw_ctx *c, const struct firmware *fw, const char *label)
{
	unsigned long i, changed = 0, shown = 0, nstr = 0;
	u8 *va = vmap(c->pages, c->npages, VM_MAP, PAGE_KERNEL);

	if (!va) {
		dev_err(c->dev, "fw-start: readback vmap failed\n");
		return;
	}
	if (!c->snap) {
		c->snap = kvmalloc(FW_DATA_VMSZ, GFP_KERNEL);
		if (c->snap) {
			c->blob.data = c->snap;
			c->blob.size = FW_DATA_VMSZ;
			c->dbg = debugfs_create_blob("ane_t8103_fw_data", 0400, NULL, &c->blob);
		}
	}
	if (c->snap)
		memcpy(c->snap, va, FW_DATA_VMSZ);
	for (i = 0; i < c->npages; i++) {
		const u8 *p = va + i * PAGE_SIZE;
		const u8 *w = i * PAGE_SIZE < FW_DATA_FSZ ? fw->data + FW_DATA_FOFF + i * PAGE_SIZE : NULL;
		unsigned long j, n = 0, run = 0;

		for (j = 0; j < PAGE_SIZE; j++)
			if (p[j] != (fill_pt && i * PAGE_SIZE + j >= PT_OFF && i * PAGE_SIZE + j < PT_OFF + PT_LEN ? 0xa5 : (w ? w[j] : 0)))
				n++;
		if (!n)
			continue;
		changed++;
		if (shown++ < 24)
			dev_info(c->dev, "fw-start: %s DATA page %lu (+%#lx) changed bytes %lu\n", label, i, i * PAGE_SIZE, n);
		for (j = 0; j <= PAGE_SIZE && nstr < 24; j++) {
			if (j < PAGE_SIZE && p[j] >= 0x20 && p[j] < 0x7f) {
				run++;
				continue;
			}
			if (run >= 10 && (!w || memcmp(p + j - run, w + j - run, run))) {
				char s[81];
				unsigned long k, m = min(run, 80UL);

				for (k = 0; k < m; k++)
					s[k] = p[j - run + k];
				s[m] = 0;
				dev_info(c->dev, "fw-start: %s str @%#lx: %s\n", label, i * PAGE_SIZE + j - run, s);
				nstr++;
			}
			run = 0;
		}
	}
	dev_info(c->dev, "fw-start: %s DATA: %lu of %lu pages differ from the payload\n", label, changed, c->npages);
	vunmap(va);
}

#define R_CPU_CONTROL	0x1400044
#define R_TICK		0x1160008
#define BOOT_READY	0x08042006U

static void wr(struct fw_ctx *c, u32 off, u32 v)
{
	writel(v, c->eng + off);
}

/* Stage 3: T6021 recipe (ane_t6021_boot_run), legacy ChMan select, no preboot table, no W8 grant tunables,
 * RVBAR already latched (no write). Ends at the READY word; ChMan publication is stage 4. */
static void fw_stage3(struct fw_ctx *c, const struct firmware *fw)
{
	u32 v = 0;
	int i;

	fw_dart_dump(c, "pre-RUN");
	dev_info(c->dev, "fw-start: S3 scratch clear + SCRATCH6=1 + SCRATCH7 pulse\n");
	for (i = 0; i < 8; i++)
		wr(c, R_SCRATCH0 + 4 * i, 0);
	wr(c, R_SCRATCH0 + 24, 1);
	wr(c, R_SCRATCH0 + 28, 1);
	wr(c, R_SCRATCH0 + 28, 0);
	dev_info(c->dev, "fw-start: S3 CPU_CONTROL <- 0\n");
	wr(c, R_CPU_CONTROL, 0);
	dev_info(c->dev, "fw-start: S3 CPU_CONTROL <- 0x10 (RUN)\n");
	wr(c, R_CPU_CONTROL, 0x10);
	for (i = 0; i < 1000; i++) {
		v = rd(c, R_SCRATCH0 + 28);
		if (v == BOOT_READY)
			break;
		usleep_range(900, 1100);
	}
	dev_info(c->dev, "fw-start: S3 SCRATCH7 %#x after %d polls (%s)\n", v, i, v == BOOT_READY ? "READY" : "no READY");
	msleep(200);
	dev_info(c->dev, "fw-start: S3 CPU_STATUS %#x SCRATCH0-7 %#x %#x %#x %#x %#x %#x %#x %#x\n", rd(c, R_CPU_STATUS),
		 rd(c, R_SCRATCH0), rd(c, R_SCRATCH0 + 4), rd(c, R_SCRATCH0 + 8), rd(c, R_SCRATCH0 + 12),
		 rd(c, R_SCRATCH0 + 16), rd(c, R_SCRATCH0 + 20), rd(c, R_SCRATCH0 + 24), rd(c, R_SCRATCH0 + 28));
	dev_info(c->dev, "fw-start: S3 mailbox A2I %#x I2A %#x tick %#x pending %#x\n", rd(c, R_MBOX_A2I),
		 rd(c, R_MBOX_I2A), rd(c, R_TICK), rd(c, R_DB_PENDING));
	fw_dart_dump(c, "post-RUN");
	if (staged)
		fw_readback_staged(c, "t+0.2s");
	else
		fw_readback(c, fw, "t+0.2s");
	msleep(1800);
	if (staged)
		fw_readback_staged(c, "t+2s");
	else
		fw_readback(c, fw, "t+2s");
	dev_info(c->dev, "fw-start: S3 later: CPU_STATUS %#x SCRATCH7 %#x tick %#x\n", rd(c, R_CPU_STATUS), rd(c, R_SCRATCH0 + 28), rd(c, R_TICK));
}

static int fw_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct firmware *fw;
	void __iomem *ps;
	struct fw_ctx *c;
	u32 act, w[ANE_PS_WORDS], rv_lo, rv_hi;
	u8 dig[32];
	int err;

	if (stage < 1 || stage > 3 || (stage == 3 && !go)) {
		dev_err(dev, "fw-start: stage 3 needs go=1; stages > 3 are not implemented\n");
		return -EINVAL;
	}
	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->dev = dev;
	platform_set_drvdata(pdev, c);

	err = request_firmware(&fw, fw_name, dev);
	if (err)
		return err;
	sha256(fw->data, fw->size, dig);
	if (fw->size != FW_SIZE || memcmp(dig, fw_sha256, sizeof(dig))) {
		dev_err(dev, "fw-start: payload is not the pinned 22G74 H13 image (size %zu)\n", fw->size);
		err = -EINVAL;
		goto rel;
	}
	err = fw_attach_genpd(c);
	if (err)
		goto rel;
	pm_runtime_enable(dev);
	err = pm_runtime_resume_and_get(dev);
	if (err < 0)
		goto disable_pm;
	c->pm_held = true;
	dev_info(dev, "fw-start: powered, settling %u ms before the first access\n", settle_ms);
	msleep(settle_ms);

	ps = ioremap_np(PS_PA, 0x38);
	if (!ps) {
		err = -ENOMEM;
		goto put_pm;
	}
	for (int i = 0; i < ANE_PS_WORDS; i++)
		w[i] = readl(ps + i * 8);
	iounmap(ps);
	act = ane_ps_aggregate(w);
	dev_info(dev, "fw-start: SET act %#x\n", act);
	if (!ane_ps_islands_on(act)) {
		err = -EBUSY;
		goto put_pm;
	}
	c->eng = ioremap_np(ENGINE_PA, ENGINE_SZ);
	if (!c->eng) {
		err = -ENOMEM;
		goto put_pm;
	}
	dev_info(dev, "fw-start: CPU_STATUS %#x\n", rd(c, R_CPU_STATUS));
	rv_lo = rd(c, R_RVBAR);
	rv_hi = rd(c, R_RVBAR + 4);
	dev_info(dev, "fw-start: RVBAR %#010x%08x (entry PA expected %#llx, bit0 latched)\n", rv_hi, rv_lo,
		 text_phys);
	dev_info(dev, "fw-start: SCRATCH0-7 %#x %#x %#x %#x %#x %#x %#x %#x\n", rd(c, R_SCRATCH0), rd(c, R_SCRATCH0 + 4),
		 rd(c, R_SCRATCH0 + 8), rd(c, R_SCRATCH0 + 12), rd(c, R_SCRATCH0 + 16), rd(c, R_SCRATCH0 + 20),
		 rd(c, R_SCRATCH0 + 24), rd(c, R_SCRATCH0 + 28));
	dev_info(dev, "fw-start: mailbox A2I %#x I2A %#x, doorbell %#x pending %#x ack %#x\n", rd(c, R_MBOX_A2I),
		 rd(c, R_MBOX_I2A), rd(c, R_DOORBELL), rd(c, R_DB_PENDING), rd(c, R_DB_ACK));
	if (!(rv_lo & 1) || (((u64)rv_hi << 32 | rv_lo) & GENMASK_ULL(47, 11)) != (text_phys & GENMASK_ULL(47, 11))) {
		dev_err(dev, "fw-start: RVBAR does not latch the expected entry\n");
		err = -ENXIO;
		goto unmap_eng;
	}
	err = fw_check_text(c, fw);
	if (err)
		goto unmap_eng;
	if (stage >= 2)
		err = staged ? fw_stage_staged(c, fw) : fw_stage_data(c, fw);
	if (err)
		goto unmap_eng;
	if (stage >= 3) {
		fw_stage3(c, fw);
		release_firmware(fw);
		return 0;
	}
	release_firmware(fw);
	dev_info(dev, "fw-start: stage %u done (no device register written)\n", stage);
	return 0;

unmap_eng:
	iounmap(c->eng);
put_pm:
	pm_runtime_put_noidle(dev);
disable_pm:
	pm_runtime_disable(dev);
	fw_detach_genpd(c);
rel:
	release_firmware(fw);
	return err;
}

static void fw_remove(struct platform_device *pdev)
{
	struct fw_ctx *c = platform_get_drvdata(pdev);

	debugfs_remove(c->dbg);
	kvfree(c->snap);
	fw_unstage_data(c);
	fw_unstage_staged(c);
	iounmap(c->eng);
	pm_runtime_disable(c->dev);
	pm_runtime_put_noidle(c->dev);
	fw_detach_genpd(c);
}

static int __maybe_unused fw_pm_nop(struct device *dev)
{
	return 0;
}

static const struct dev_pm_ops fw_pm_ops = {
	SET_RUNTIME_PM_OPS(fw_pm_nop, fw_pm_nop, NULL)
};

static const struct of_device_id fw_of_match[] = {
	{ .compatible = "apple,t8103-ane" },
	{}
};
MODULE_DEVICE_TABLE(of, fw_of_match);

static struct platform_driver fw_driver = {
	.probe = fw_probe,
	.remove = fw_remove,
	.driver = {
		.name = "ane_t8103_fw",
		.suppress_bind_attrs = true,
		.pm = pm_ptr(&fw_pm_ops),
		.of_match_table = fw_of_match,
	},
};
module_platform_driver(fw_driver);

MODULE_DESCRIPTION("T8103 ANE firmware start, stages 1-2 (read-only checks and DATA staging)");
MODULE_LICENSE("Dual MIT/GPL");
