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
	0xf9000006,	/* store patched entry word */
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

/* fw_trace=2: breadcrumbs. Each of these instructions (none PC-relative except the entry `b`, none touching x28/x29, which the entry code
 * never uses) is moved into a 5-word trampoline in vector padding that first stores its 1-based index to trace page +0x40 + 8*(index-1)
 * with x28/x29, then runs the moved instruction and branches back (the entry branch at 0 goes straight to its target). The host
 * reads which markers appeared. */
static const struct { u32 at, orig; } ane_h15_crumb[] = {
	{ 0x000, 0x14000081 }, { 0x204, 0xd5384241 }, { 0x228, 0xd50343df }, { 0x274, 0x9a800400 },
	{ 0x2ec, 0x91002000 }, { 0x378, 0xaa0003f8 }, { 0x384, 0xf9000022 }, { 0x38c, 0x9108c000 },
	{ 0x414, 0xf100041f }, { 0x44c, 0xd5182040 }, { 0x4d4, 0xd2800000 }, { 0x590, 0xd5181000 },
};
#define ANE_H15_NCRUMB ARRAY_SIZE(ane_h15_crumb)
#define ANE_H15_CRUMB_OFF 0x40u

/* Trampoline home of crumb i: vector padding not used by the trace stubs, 0x94..0xff, 0x114..0x17f, 0x1d0..0x1ff (5, 5, 2 trampolines). */
static u32 ane_h15_crumb_tramp(unsigned int i)
{
	return i < 5 ? 0x94 + 20 * i : i < 10 ? 0x114 + 20 * (i - 5) : 0x1d0 + 20 * (i - 10);
}

static int ane_h15_patch_crumbs(void *image)
{
	u32 *t = image;
	unsigned int i, w;

	for (i = 0; i < ANE_H15_NCRUMB; i++) {
		if (le32_to_cpu(t[ane_h15_crumb[i].at / 4]) != ane_h15_crumb[i].orig)
			return -EBADMSG;
		for (w = 0; w < 5; w++)
			if (t[ane_h15_crumb_tramp(i) / 4 + w])
				return -EBADMSG;
	}
	for (i = 0; i < ANE_H15_NCRUMB; i++) {
		u32 tp = ane_h15_crumb_tramp(i), at = ane_h15_crumb[i].at;

		t[tp / 4 + 0] = cpu_to_le32(0x9000181c);			/* adrp x28, 0x300000 */
		t[tp / 4 + 1] = cpu_to_le32(0xd280001d | ((i + 1) << 5));	/* mov x29, #index */
		t[tp / 4 + 2] = cpu_to_le32(0xf9000000 | (((ANE_H15_CRUMB_OFF / 8) + i) << 10) | (28 << 5) | 29); /* str x29, [x28, #off] */
		if (at == 0) {
			t[tp / 4 + 3] = cpu_to_le32(0x14000000 | (((0x204 - (tp + 12)) / 4) & 0x3ffffff));	/* the entry b's target */
		} else {
			t[tp / 4 + 3] = cpu_to_le32(ane_h15_crumb[i].orig);
			t[tp / 4 + 4] = cpu_to_le32(0x14000000 | (((at + 4 - (tp + 16)) / 4) & 0x3ffffff));
		}
		t[at / 4] = cpu_to_le32(0x14000000 | (((tp - at) / 4) & 0x3ffffff));
	}
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
		if (!ret && fw_trace > 1)
			ret = ane_h15_patch_crumbs(image.va);
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
