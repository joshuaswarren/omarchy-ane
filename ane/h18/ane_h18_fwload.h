/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef ANE_H18_FWLOAD_H
#define ANE_H18_FWLOAD_H

#include "ane_h18_fw_pins.h"
#include "ane_h18_fw_layout.h"

/* Linux-owned image preparation for the T8152 ANE0, included by ane_h18_main.c after its
 * helpers. Structure follows the accepted H15 kernel-residency pattern (ane/h15/ane_h15_main.c
 * 959..1049 unmap_range/map_owned_segment and ane_h15_fwload.h) with the H18 fixed contract
 * only: exact 26A428 payload pin, zeroed 0x370000 coherent image, TEXT/DATA copied byte-exact,
 * nothing patched, entry 0. The image is staged unmodified: no ECAP seed, no patchbay or
 * tunables rewrite, no PAC/security change — whether the firmware needs producer-applied
 * initialization is experimentally unqualified, and this trial publishes the shipped bytes.
 * No H15 patch offset, wrapper write or alias policy is carried over.
 * The hash helper is sha256() (kernel: crypto/sha2.h; sim: ane_sha256 via sim/shim.h). */

/* Short-unmap retention (H15 959..975): a partial iommu_unmap keeps the domain, the image and
 * the module pin; no retry, no unload, reboot required. Never called while the core may run. */
static bool ane_h18_fw_unmap_range(struct ane_h18 *ane, u64 iova, u64 size)
{
	size_t unmapped;

	if (ane->map_retained)
		return false;
	unmapped = iommu_unmap(ane->domain, iova, size);
	if (unmapped != size) {
		ane->map_retained = true;
		dev_emerg(ane->dev,
			  "ane_h18 incomplete owned firmware unmap iova=%#llx size=%#llx unmapped=%#zx: retaining domain, image and module; reboot required\n",
			  iova, size, unmapped);
		return false;
	}
	return true;
}

/* Undo the owned map and release the owned backing in the required order: both segment aliases
 * unmap BEFORE the image frees; a short unmap retains everything and frees nothing. */
static void ane_h18_unmap_owned_firmware(struct ane_h18 *ane)
{
	unsigned int i;

	if (!ane->domain || ane->map_retained)
		return;
	for (i = 0; i < ane->map_n; i++)
		if (!ane_h18_fw_unmap_range(ane, ane->map_iova[i], ane->map_len[i]))
			return;
	ane->map_n = 0;
	if (ane->owned_fw_img.va) {
		dma_free_coherent(ane->dev, ane->owned_fw_img.size, ane->owned_fw_img.va,
				  ane->owned_fw_img.iova);
		ane->owned_fw_img.va = NULL;
	}
	if (ane->dom_own) {
		iommu_detach_device(ane->domain, ane->dev);
		iommu_domain_free(ane->domain);
	}
	ane->domain = NULL;
	ane->dom_own = false;
}

/* Stage the pinned payload into Linux-owned coherent memory. Exact size and the exact
 * whole-payload SHA-256 must match before anything is allocated; a busy owner refuses; the
 * copy never aliases or mutates the payload and never writes anything beyond TEXT and DATA. */
static int ane_h18_prepare_owned_firmware(struct ane_h18 *ane)
{
	const struct firmware *fw;
	u8 digest[32];
	int ret;

	if (ane->owned_fw_img.va)
		return -EBUSY;
	ret = request_firmware(&fw, fw_path, ane->dev);
	if (ret)
		return ret;
	if (fw->size != ane_fw_t8152_ane0.size) {
		dev_err(ane->dev, "ane_h18 owned fw: %s is %zu bytes, the pin holds %u\n",
			fw_path, fw->size, ane_fw_t8152_ane0.size);
		ret = -EINVAL;
		goto release;
	}
	sha256(fw->data, fw->size, digest);
	if (memcmp(digest, ane_fw_t8152_ane0.sha256, sizeof(digest))) {
		dev_err(ane->dev, "ane_h18 owned fw: %s does not match the pinned payload digest\n", fw_path);
		ret = -EBADMSG;
		goto release;
	}
	ret = dma_set_mask_and_coherent(ane->dev, DMA_BIT_MASK(42));
	if (ret)
		goto release;
	ane->owned_fw_img.size = ANE_H18_FW_IMAGE_SIZE;
	ane->owned_fw_img.va = dma_alloc_coherent(ane->dev, ane->owned_fw_img.size,
						  &ane->owned_fw_img.iova, GFP_KERNEL);
	if (!ane->owned_fw_img.va) {
		ret = -ENOMEM;
		goto release;
	}
	memset(ane->owned_fw_img.va, 0, ANE_H18_FW_IMAGE_SIZE);
	ane_h18_fw_copy(ane->owned_fw_img.va, fw->data);
	dma_wmb();
	dev_info(ane->dev,
		 "ane_h18 owned firmware staged bytes=%#x dma=%#llx from \"%s\"; image unmodified, not published or started\n",
		 ANE_H18_FW_IMAGE_SIZE, (u64)ane->owned_fw_img.iova, fw_path);
	ret = 0;
release:
	release_firmware(fw);
	return ret;
}

/* Read each 16 KiB page's physical address from the default DMA domain now, before any attach
 * replaces it; the coherent pages are not necessarily contiguous (H15 985..1002). A zero or
 * misaligned page address refuses the trial before any map. */
static int ane_h18_plan_owned_map(struct ane_h18 *ane)
{
	struct iommu_domain *dom;
	u64 off;
	int ret = ane_h18_prepare_owned_firmware(ane);

	if (ret)
		return ret;
	dom = iommu_get_domain_for_dev(ane->dev);
	if (!dom || !(dom->type & __IOMMU_DOMAIN_DMA_API)) {
		dev_err(ane->dev, "ane_h18 owned fw: the ANE has no default DMA domain\n");
		return -ENODEV;
	}
	for (off = 0; off < ANE_H18_FW_IMAGE_SIZE; off += SZ_16K) {
		u64 pa = iommu_iova_to_phys(dom, ane->owned_fw_img.iova + off);

		if (!pa || (pa & (SZ_16K - 1))) {
			dev_err(ane->dev, "ane_h18 owned fw: image page at +%#llx has bad physical address %#llx\n", off, pa);
			return -EIO;
		}
		ane->owned_pa[off / SZ_16K] = pa;
	}
	return 0;
}

/* Map one segment of the owned image in physically contiguous runs; a failure unmaps every run
 * of this segment mapped so far, and each mapped run is verified by readback (H15 1021..1049). */
static int ane_h18_map_owned_segment(struct ane_h18 *ane, struct iommu_domain *dom, unsigned int seg)
{
	const u64 first = seg ? ANE_H18_FW_TEXT_SIZE : 0;
	const u64 end = first + ane->map_size[seg];
	const int prot = IOMMU_READ | (seg ? IOMMU_WRITE : 0);
	u64 off = first;
	int ret = 0;

	while (off < end) {
		u64 pa = ane->owned_pa[off / SZ_16K], len = SZ_16K, iova = ane->map_iova[seg] + off - first;

		while (off + len < end && ane->owned_pa[(off + len) / SZ_16K] == pa + len)
			len += SZ_16K;
		ret = iommu_map(dom, iova, pa, len, prot, GFP_KERNEL);
		if (ret)
			break;
		off += len;
		ane->map_len[seg] = off - first;
		if (iommu_iova_to_phys(dom, iova) != pa) {
			ret = -EIO;
			break;
		}
	}
	if (ret) {
		if (off > first)
			ane_h18_fw_unmap_range(ane, ane->map_iova[seg], off - first);
		ane->map_len[seg] = 0;	/* the failing segment holds nothing: never unmap it again */
	} else {
		ane->map_len[seg] = ane->map_size[seg];
	}
	return ret;
}

/* Release the staged image when nothing is mapped and no domain is held (a pre-RUN prepare/plan failure). */
static void ane_h18_free_owned_image(struct ane_h18 *ane)
{
	if (!ane->owned_fw_img.va || ane->domain || ane->map_n)
		return;
	dma_free_coherent(ane->dev, ane->owned_fw_img.size, ane->owned_fw_img.va, ane->owned_fw_img.iova);
	ane->owned_fw_img.va = NULL;
}

#endif /* ANE_H18_FWLOAD_H */
