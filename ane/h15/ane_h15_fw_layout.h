/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef ANE_H15_FW_LAYOUT_H
#define ANE_H15_FW_LAYOUT_H

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

#endif
