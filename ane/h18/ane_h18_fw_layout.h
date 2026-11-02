/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef ANE_H18_FW_LAYOUT_H
#define ANE_H18_FW_LAYOUT_H

#include <string.h>

/* ARM64E MH_PRELOAD segments of the pinned payload (data/ane-soc/t8152.json):
 * __TEXT file 0x4000 size 0xc4000 -> vm 0; __DATA file 0xc8000 file size
 * 0xb8000 -> vm 0xc4000 vmsize 0x2ac000; __DATA_CONST empty (vm 0x370000,
 * vmsize 0). Owned image footprint 0x370000, entry 0. */
#define ANE_H18_FW_TEXT_FILE 0x4000u
#define ANE_H18_FW_TEXT_SIZE 0xc4000u
#define ANE_H18_FW_DATA_FILE 0xc8000u
#define ANE_H18_FW_DATA_FILE_SIZE 0xb8000u
#define ANE_H18_FW_IMAGE_SIZE 0x370000u

/* Call only after the complete payload pin matches and the owner is zeroed. */
static inline void ane_h18_fw_copy(void *storage, const void *payload)
{
	const unsigned char *bytes = payload;

	memcpy(storage, bytes + ANE_H18_FW_TEXT_FILE, ANE_H18_FW_TEXT_SIZE);
	memcpy((unsigned char *)storage + ANE_H18_FW_TEXT_SIZE,
	       bytes + ANE_H18_FW_DATA_FILE, ANE_H18_FW_DATA_FILE_SIZE);
}

#endif
