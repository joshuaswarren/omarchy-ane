/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef ANE_H15_FWLOAD_H
#define ANE_H15_FWLOAD_H

static int ane_h15_prepare_owned_firmware(struct ane_h15 *ane)
{
	const struct ane_h15_soc *s = ane->soc;
	const struct firmware *fw;
	struct iommu_domain *domain;
	struct ane_h15_hs_buf image = { 0 };
	u8 digest[32];
	u32 status, control;
	u64 wrapper;
	int ret;

	if (ane->owned_fw.va)
		return -EBUSY;
	if (!confirm_read)
		return -EPERM;
	if (!ane_h15_window_ok(ane, 0, "engine", s->engine_pa, s->engine_size) ||
	    !ane_h15_window_ok(ane, 1, "pmgr", s->pmgr_pa, s->pmgr_size) ||
	    ANE_H15_WRAP_OFF + ANE_H15_WRAP_SIZE > s->engine_size ||
	    s->engine_pa + ANE_H15_WRAP_OFF < s->engine_pa)
		return -EINVAL;
	ret = ane_h15_ps_wait(ane);
	if (!ret)
		ret = ane_h15_read_word(ane, "CPU_STATUS", s->cpu_status, &status);
	if (!ret)
		ret = ane_h15_read_ctrl(ane, &control);
	if (ret)
		return ret;
	if (!ane_h15_core_stopped(status) || (control & ANE_H15_CPU_RUN))
		return -EBUSY;
	domain = iommu_get_domain_for_dev(ane->dev);
	if (!domain || !(domain->type & __IOMMU_DOMAIN_DMA_API))
		return -ENODEV;
	ret = request_firmware(&fw, fw_path, ane->dev);
	if (ret)
		return ret;
	if (fw->size != s->fw->size) {
		ret = -EINVAL;
		goto release;
	}
	sha256(fw->data, fw->size, digest);
	if (memcmp(digest, s->fw->sha256, sizeof(digest))) {
		ret = -EBADMSG;
		goto release;
	}
	ret = dma_set_mask_and_coherent(ane->dev, DMA_BIT_MASK(64));
	if (ret)
		goto release;
	image.size = ANE_H15_FW_IMAGE_SIZE;
	image.va = dma_alloc_coherent(ane->dev, image.size, &image.iova, GFP_KERNEL);
	if (!image.va) {
		ret = -ENOMEM;
		goto release;
	}
	memset(image.va, 0, image.size);
	ane_h15_fw_copy(image.va, fw->data);
	wrapper = s->engine_pa + ANE_H15_WRAP_OFF;
	if (wrad_patch)
		put_unaligned_le64(wrapper, (u8 *)image.va + s->fw->wrad_vm);
	dma_wmb();
	ane->owned_fw = image;
	dev_info(ane->dev,
		 "ane_h15 owned firmware prepared bytes=%#zx dma=%#llx wrapper=%#llx; not published or started\n",
		 image.size, (u64)image.iova, wrapper);
	ret = 0;
release:
	release_firmware(fw);
	return ret;
}

#endif
