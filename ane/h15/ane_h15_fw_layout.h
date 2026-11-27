/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef ANE_H15_FW_LAYOUT_H
#define ANE_H15_FW_LAYOUT_H

#include <linux/types.h>

/* Firmware segment layouts for the H15 family.
 *
 * Legacy: the macOS < 27.2 image this module first shipped with. TEXT is
 * carved from payload offset 0x4000 (size 0xcc000, mapped read-only), DATA
 * follows at payload offset 0xd0000, and both segments map contiguously:
 * TEXT at the RVBAR entry IOVA, DATA directly behind it.
 *
 * macOS 27.2 (26B123): the TEXT carve-out moves to 0xc8000 and the DATA
 * segment to payload offset 0x2a4000; DATA maps at its own IOVA base,
 * 0x100000c8000 (read from the live device tree's memory-region property
 * when present — see ane_h15_dt_memory_region()). The pins below encode
 * the layout facts observed on macOS 27.2; the image-size and DATA-length
 * pins for this generation are pinned from the first 26B123 image at the
 * silicon run and are intentionally not guessed here.
 */
struct ane_h15_fw_layout {
	const char *name;
	u32 text_file;		/* payload offset of the TEXT segment */
	u32 text_size;		/* TEXT carve-out (the read-only mapping) */
	u32 data_file;		/* payload offset of the DATA segment */
	u32 data_file_size;	/* DATA bytes carried in the image (0 = to the
				 * end of the mapped image; 27.2 pins it at
				 * the silicon run) */
	u32 image_size;		/* the whole mapped image (legacy value until
				 * the 27.2 image pins it) */
	u64 data_iova;		/* DATA segment IOVA base (0 = TEXT + TEXT size) */
};

/* The legacy compile-time pins: kept for the owned-page array sizing and
 * as the fallback extent while the 27.2 image pins are pending. */
#define ANE_H15_FW_TEXT_FILE 0x4000u
#define ANE_H15_FW_TEXT_SIZE 0xcc000u
#define ANE_H15_FW_DATA_FILE 0xd0000u
#define ANE_H15_FW_DATA_FILE_SIZE 0xb8000u
#define ANE_H15_FW_IMAGE_SIZE 0x36c000u

/* Call only after the complete payload pin matches and the owner is zeroed. */
static inline void ane_h15_fw_copy(void *storage, const void *payload)
{
	const unsigned char *bytes = payload;

	memcpy(storage, bytes + ANE_H15_FW_TEXT_FILE, ANE_H15_FW_TEXT_SIZE);
	memcpy((unsigned char *)storage + ANE_H15_FW_TEXT_SIZE,
	       bytes + ANE_H15_FW_DATA_FILE, ANE_H15_FW_DATA_FILE_SIZE);
}

/* The legacy pins are preserved verbatim: every assert in
 * tests/test_owned_firmware.c and every shipped boot ran against them. */
#define ANE_H15_FW_LAYOUT_LEGACY_NAME "legacy"
static const struct ane_h15_fw_layout ane_h15_fw_layout_legacy = {
	.name = ANE_H15_FW_LAYOUT_LEGACY_NAME,
	.text_file = 0x4000u,
	.text_size = 0xcc000u,
	.data_file = 0xd0000u,
	.data_file_size = 0xb8000u,
	.image_size = 0x36c000u,
	.data_iova = 0,
};

/* macOS 27.2 (26B123): TEXT=0xc8000, DATA=0x2a4000,
 * DATA IOVA=0x100000c8000. */
#define ANE_H15_FW_LAYOUT_MACOS_27_2_NAME "macos-27.2"
static const struct ane_h15_fw_layout ane_h15_fw_layout_macos_27_2 = {
	.name = ANE_H15_FW_LAYOUT_MACOS_27_2_NAME,
	.text_file = 0x4000u,
	.text_size = 0xc8000u,
	.data_file = 0x2a4000u,
	.data_file_size = 0u,	/* pinned from the first 26B123 image */
	.image_size = 0u,	/* ditto */
	.data_iova = 0x100000c8000ull,
};

#endif /* ANE_H15_FW_LAYOUT_H */
