// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * T6021 ANE firmware loader — W13 increment (receipt
 * 2026-09-19-h14-w13-boot-contract.md §5-§7).
 *
 * Implements, behind fw_load=1:
 *   1. request_firmware("apple/ane/t602x_ane0_fw_selene_rc4x.macho")
 *   2. Validation via ane_fw_validate.h (shared, offline-regressed by
 *      tools/h14_fwload_regression.c): sha256 pin, strict exact-image
 *      assertions (7 load commands, 3 pinned segments, entry 0,
 *      bounded LC walk).
 *   3. dma_alloc_coherent on the ANE platform device: the buffer is
 *      DART-mapped through the device's iommu group. NOTE: mapped-
 *      host-visible only; equality of this iova with the FIRMWARE's
 *      address space is NOT established until the dart-ane0 stream
 *      mapping is verified (W13 review) — treat the iova as data,
 *      not as a boot constant.
 *
 * NOT implemented: the boot step (RVBAR write + SCRATCH7). The
 * surface-address publication path to the boot ROM is unevidenced; any
 * boot write before that datum would be exactly the blind retry the
 * lane rules forbid.
 *
 * Load is non-fatal to probe: the W10 transport role of this driver is
 * independent of firmware presence.
 */
#include <crypto/sha2.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/moduleparam.h>
#include <linux/sizes.h>
#include <linux/unaligned.h>

#include "ane_t6021.h"
#include "ane_fw_validate.h"

static bool fw_load;
module_param(fw_load, bool, 0444);
MODULE_PARM_DESC(fw_load,
		 "OPT-IN: validate + DART-map the selene PRELOAD payload "
		 "(W13). No boot action; publication datum unevidenced.");

#define ANE_FW_NAME "apple/ane/t602x_ane0_fw_selene_rc4x.macho"

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

	ret = dma_set_mask_and_coherent(ane->dev, DMA_BIT_MASK(64));
	if (ret) {
		dev_err(ane->dev, "fwload: dma mask: %d\n", ret);
		return ret;
	}

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
		 "entry %#llx, iova %pad size %#x (no boot action — "
		 "publication datum unevidenced, W13)\n",
		 entry, &iova, ANE_FW_BUF_SIZE);
	release_firmware(fw);
	return 0;
}

void ane_t6021_fwload_remove(struct ane_t6021 *ane)
{
	if (!ane->fw_buf)
		return;
	dma_free_coherent(ane->dev, ane->fw_size, ane->fw_buf, ane->fw_iova);
	ane->fw_buf = NULL;
}
