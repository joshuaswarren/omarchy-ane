// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * T6021 ANE firmware loader — W13/W14 increment (receipts
 * 2026-09-19-h14-w13-boot-contract.md, -w14-staging-proven.md).
 *
 * Implements, behind fw_load=1:
 *   1. request_firmware("apple/ane/t602x_ane0_fw_selene_rc4x.macho")
 *   2. Validation via ane_fw_validate.h (shared with the offline
 *      regression h14_fwload_regression.c, shipped in
 *      ane-linux-experiments/tools/): sha256 pin, strict
 *      exact-image assertions (7 load commands, 3 pinned segments,
 *      entry 0, bounded LC walk).
 *   3. dma_alloc_coherent on the ANE platform device: the buffer is
 *      DART-mapped through the device's iommu group (stream 0 of the
 *      three bound instances, H14DartAudit). Segment-wise copy
 *      (__TEXT fileoff 0x4000 -> vm0, __DATA -> 0xe8000; vmsize tail
 *      zero from the coherent alloc). Coherent memory needs no explicit
 *      cache clean.
 *   4. W16 entry alias: the staged fw pages are ALIASED at the latched
 *      RVBAR entry on the device's default DMA domain. Placement
 *      contract, receipt receipts/2026-09-20-t6021-entry-alias.md:
 *        - live RVBAR read64 = 0x10000000001 (bit0 latched, entry
 *          bits 0x10000000000 = dart-ane vm base); kext law
 *          (rvbar-lifecycle 6288b0b): bit0 set => the RVBAR write
 *          branch is skipped, so the ASC fetches AT the latched entry;
 *        - netconsole 2026-09-20T17:48:19: apple-dart 285800000.iommu
 *          "translation fault ... code:0x2 (NO PGD FOR IOVA) at
 *          0x100000dca10" from the pre-module userspace CPU release —
 *          the ASC fetched the entry region while nothing was mapped;
 *        - current-state table walk (kcore, 2026-09-20): entry
 *          pgd[16] = 0 while the fw leaves are valid -> the region the
 *          hardware fetches is unmapped and the fw surface is
 *          physically NON-contiguous (consecutive leaf PAs differ by
 *          -0x4000). The alias therefore copies each page's PA from
 *          the live fw translation (iommu_iova_to_phys), never from
 *          the kernel VA, and verifies every target page unmapped
 *          before mapping it.
 *      Driver-managed only: iommu_map() on the attached domain —
 *      apple_dart implements .map_pages + .iotlb_sync_map, so the
 *      driver flushes the DART TLB; no raw TTBR/TCR writes.
 *      Collision ceiling: the iovad allocates top-down from ~4 TiB
 *      and total mappable RAM is 94.9 GiB (/proc/iomem), so the
 *      allocator floor 4 TiB - 96 GiB = 0x3e800000000 is far above
 *      the alias ceiling 0x10005000000; the per-page zero-check
 *      additionally guards each map.
 *
 * NOT implemented: the boot step (CPU release + SCRATCH). The userspace
 * hybrid stage owns the W8 grant writes; with the alias in place a
 * release no longer faults the entry fetch.
 *
 * Error ownership: every failure path releases the firmware and, once
 * allocated, the coherent buffer, and clears ane->fw_buf — remove()
 * frees only what fw_buf still names. Load is non-fatal to probe.
 */
#include <crypto/sha2.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/moduleparam.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>

#include "ane_t6021.h"
#include "ane_t6021_boot.h"
#include "ane_fw_validate.h"

static bool fw_load;
module_param(fw_load, bool, 0444);
MODULE_PARM_DESC(fw_load,
		 "OPT-IN: validate + DART-map the selene PRELOAD payload "
		 "(W13/W14). No boot action; publication datum unevidenced.");

#define ANE_FW_NAME "apple/ane/t602x_ane0_fw_selene_rc4x.macho"

/* DART page size on t6021 (apple_dart probe line: "pagesize 4000");
 * fw_size is a multiple. */
#define ANE_T6021_FW_ALIAS_PAGE	0x4000

static int ane_t6021_fw_alias_map(struct ane_t6021 *ane)
{
	struct iommu_domain *dom = iommu_get_domain_for_dev(ane->dev);
	void __iomem *eng = ane->base[ANE_T6021_REG_ENGINE];
	u64 rvbar = readq(eng + ANE_ASC_RVBAR);
	u64 entry = ane_t6021_rvbar_entry_bits(rvbar);
	phys_addr_t pa0 = 0;
	int prot = IOMMU_READ | IOMMU_WRITE;
	u64 off;
	int ret;

	if (!ane_t6021_rvbar_latched(rvbar) || !entry) {
		/* Unlatched branch: the boot path programs RVBAR to the
		 * fw DVA itself (ane_t6021_rvbar_compose), no alias. */
		dev_info(ane->dev,
			 "fwalias: rvbar %016llx not latched/entry 0 — skip (boot reprograms RVBAR)\n",
			 rvbar);
		return 0;
	}
	if (!ane_t6021_rvbar_entry_ok(entry) ||
	    entry & (ANE_T6021_FW_ALIAS_PAGE - 1)) {
		dev_err(ane->dev,
			"fwalias: entry %#llx not %#x-aligned in fold\n",
			entry, ANE_T6021_FW_ALIAS_PAGE);
		return -EINVAL;
	}
	if (!dom ||
	    entry + ane->fw_size - 1 > dom->geometry.aperture_end) {
		dev_err(ane->dev,
			"fwalias: entry %#llx+%#x outside aperture %#llx\n",
			entry, ane->fw_size,
			(unsigned long long)dom->geometry.aperture_end);
		return dom ? -ERANGE : -ENODEV;
	}
	if (dev_is_dma_coherent(ane->dev))
		prot |= IOMMU_CACHE;

	for (off = 0; off < ane->fw_size; off += ANE_T6021_FW_ALIAS_PAGE) {
		phys_addr_t pa = iommu_iova_to_phys(dom, ane->fw_iova + off);

		if (!pa || pa & (ANE_T6021_FW_ALIAS_PAGE - 1)) {
			dev_err(ane->dev,
				"fwalias: fw +%#llx untranslated (%pa)\n",
				off, &pa);
			ret = -EFAULT;
			goto err_unmap;
		}
		if (iommu_iova_to_phys(dom, entry + off)) {
			dev_err(ane->dev,
				"fwalias: entry +%#llx already mapped — refusing\n",
				off);
			ret = -EEXIST;
			goto err_unmap;
		}
		ret = iommu_map(dom, entry + off, pa,
				ANE_T6021_FW_ALIAS_PAGE, prot, GFP_KERNEL);
		if (ret) {
			dev_err(ane->dev, "fwalias: iommu_map +%#llx: %d\n",
				off, ret);
			goto err_unmap;
		}
		if (!off)
			pa0 = pa;
	}

	if (iommu_iova_to_phys(dom, entry) != pa0) {
		dev_err(ane->dev, "fwalias: iova_to_phys roundtrip mismatch\n");
		ret = -EIO;
		goto err_unmap;
	}

	ane->fw_alias_iova = entry;
	dev_info(ane->dev,
		 "fwalias: entry %#llx <- %u dart pages aliased from fw %pad (first %pa, roundtrip OK)\n",
		 entry, ane->fw_size / ANE_T6021_FW_ALIAS_PAGE,
		 &ane->fw_iova, &pa0);
	return 0;

err_unmap:
	if (off)
		iommu_unmap(dom, entry, off);
	return ret;
}

static const u8 ane_fw_sha256_expected[32] = {
	0x9f, 0x79, 0x15, 0xc4, 0x31, 0xd2, 0x88, 0xa2,
	0xbd, 0xc2, 0x13, 0x2c, 0x39, 0x9d, 0xb8, 0xcf,
	0x55, 0x74, 0x71, 0x6a, 0x3b, 0x1e, 0x94, 0xaf,
	0x76, 0xbe, 0x6a, 0x29, 0x1c, 0x2e, 0x66, 0x5b,
};

int ane_t6021_fwload_probe(struct ane_t6021 *ane)
{
	const struct firmware *fw = NULL;
	struct ane_fw_seg segs[ANE_FW_NSEGS];
	u64 entry = 0;
	u8 actual_sha[32];
	void *buf;
	dma_addr_t iova;
	unsigned int i;
	const char *reason = NULL;
	int ret;

	if (!fw_load)
		return 0;

	/* The 64-bit coherent mask is set once in ane_t6021_probe,
	 * BEFORE rtkit_init allocates the rings (W15 review). */

	ret = request_firmware(&fw, ANE_FW_NAME, ane->dev);
	if (ret) {
		dev_err(ane->dev,
			"fwload: request_firmware(%s): %d — stage the payload "
			"under /lib/firmware/apple/ane/\n", ANE_FW_NAME, ret);
		return ret;
	}

	sha256(fw->data, fw->size, actual_sha);
	ret = ane_fw_validate_blob(fw->data, fw->size,
				   ane_fw_sha256_expected, actual_sha,
				   segs, &entry, &reason);
	if (ret) {
		dev_err(ane->dev, "fwload: validation failed: %s\n",
			reason ? reason : "?");
		release_firmware(fw);
		return ret;
	}

	buf = dma_alloc_coherent(ane->dev, ANE_FW_BUF_SIZE, &iova, GFP_KERNEL);
	if (!buf) {
		dev_err(ane->dev, "fwload: coherent alloc %#x failed\n",
			ANE_FW_BUF_SIZE);
		release_firmware(fw);
		return -ENOMEM;
	}

	for (i = 0; i < ANE_FW_NSEGS; i++) {
		if (segs[i].filesize)
			memcpy(buf + segs[i].vmaddr,
			       fw->data + segs[i].fileoff, segs[i].filesize);
	}

	ane->fw_buf = buf;
	ane->fw_iova = iova;
	ane->fw_size = ANE_FW_BUF_SIZE;

	dev_info(ane->dev,
		 "fwload: selene PRELOAD validated + DART-mapped: 3 segs, "
		 "entry %#llx, iova %pad size %#x\n",
		 entry, &iova, ANE_FW_BUF_SIZE);

	ret = ane_t6021_fw_alias_map(ane);
	if (ret) {
		ane_t6021_fwload_remove(ane);
		release_firmware(fw);
		return ret;
	}

	release_firmware(fw);
	return 0;
}

void ane_t6021_fwload_remove(struct ane_t6021 *ane)
{
	if (ane->fw_alias_iova) {
		struct iommu_domain *dom = iommu_get_domain_for_dev(ane->dev);

		if (dom)
			iommu_unmap(dom, ane->fw_alias_iova, ane->fw_size);
		ane->fw_alias_iova = 0;
	}
	if (!ane->fw_buf)
		return;
	dma_free_coherent(ane->dev, ane->fw_size, ane->fw_buf, ane->fw_iova);
	ane->fw_buf = NULL;
	ane->fw_size = 0;
}
