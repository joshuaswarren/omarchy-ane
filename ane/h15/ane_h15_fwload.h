/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef ANE_H15_FWLOAD_H
#define ANE_H15_FWLOAD_H

/* fw_trace: the firmware's exception vectors spin on `b .`. Turn each into a stub that records ESR/FAR/ELR/SPSR_EL1 and the vector
 * number into a page of the zero BSS tail (vm ANE_H15_FW_TRACE_VM, which the host reads back), then spins. The stubs live in the
 * vectors' own udf padding at text 0x190..0x1ff; slot 0x200 (`b .`, EL1h sync) branches to the stub at 0x194. Refuses an image whose
 * words are not exactly what was disassembled from the 26A428 payload. */
#define ANE_H15_FW_TRACE_VM 0x300000u
static const u32 ane_h15_trace_stub[] = {		/* text 0x1a0.. */
	0x90001800,	/* adrp x0, 0x300000 (pc page 0) */
	0xd5385202,	/* mrs x2, esr_el1 */
	0xd5386003,	/* mrs x3, far_el1 */
	0xd5384024,	/* mrs x4, elr_el1 */
	0xd5384005,	/* mrs x5, spsr_el1 */
	0xa9010c02,	/* stp x2, x3, [x0, #0x10] */
	0xa9021404,	/* stp x4, x5, [x0, #0x20] */
	0xf9000401,	/* str x1, [x0, #8] */
	0xd28a08a6,	/* mov x6, #0x5045 */
	0xf9000006,	/* str x6, [x0] */
	0xd5033f9f,	/* dsb sy */
	0x14000000,	/* b . */
};

static int ane_h15_patch_vectors(void *image)
{
	u32 *t = image;
	static const unsigned int bdot[] = { 0x8c, 0x10c, 0x18c, 0x200 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(bdot); i++)
		if (le32_to_cpu(t[bdot[i] / 4]) != 0x14000000)
			return -EBADMSG;
	for (i = 0x190 / 4; i < 0x200 / 4; i++)
		if (t[i])
			return -EBADMSG;
	/* the slots' udf padding reads as zero words (udf #0) */
	t[0x8c / 4] = cpu_to_le32(0x52800021);			/* mov w1, #1 */
	t[0x90 / 4] = cpu_to_le32(0x14000000 | ((0x1a0 - 0x90) / 4));
	t[0x10c / 4] = cpu_to_le32(0x52800041);			/* mov w1, #2 */
	t[0x110 / 4] = cpu_to_le32(0x14000000 | ((0x1a0 - 0x110) / 4));
	t[0x18c / 4] = cpu_to_le32(0x52800061);			/* mov w1, #3 */
	t[0x190 / 4] = cpu_to_le32(0x14000000 | ((0x1a0 - 0x190) / 4));
	t[0x194 / 4] = cpu_to_le32(0x52800081);			/* mov w1, #4 (slot 0x200) */
	t[0x198 / 4] = cpu_to_le32(0x14000000 | ((0x1a0 - 0x198) / 4));
	t[0x200 / 4] = cpu_to_le32(0x14000000 | ((0x194 - 0x200) / 4 & 0x3ffffff));
	for (i = 0; i < ARRAY_SIZE(ane_h15_trace_stub); i++)
		t[0x1a0 / 4 + i] = cpu_to_le32(ane_h15_trace_stub[i]);
	return 0;
}

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
	if (fw_trace) {
		ret = ane_h15_patch_vectors(image.va);
		if (ret) {
			dma_free_coherent(ane->dev, image.size, image.va, image.iova);
			goto release;
		}
	}
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
