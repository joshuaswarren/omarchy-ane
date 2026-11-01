// SPDX-License-Identifier: MIT
#include "ane_h15.h"
#include "ane_sha256.h"
#include "../ane/h15/ane_h15_fw_pins.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* The full-image hash binds this measured layout; no untrusted offsets are used. */
enum {
	H15_TEXT_FILE = 0x4000,
	H15_TEXT_SIZE = 0xcc000,
	H15_DATA_FILE = 0xd0000,
	H15_DATA_FILE_SIZE = 0xb8000,
	H15_IMAGE_SIZE = 0x36c000
};

static const struct ane_h15_fw *firmware_for_soc(uint32_t soc)
{
	switch (soc) {
	case 0x8122:
		return &ane_fw_t8122;
	case 0x6030:
		return &ane_fw_t6030;
	case 0x6031:
	case 0x6034:
		return &ane_fw_t6031;
	default:
		return NULL;
	}
}

int ane_h15_firmware_stage(uint32_t soc, const void *payload, uint64_t size,
			   struct ane_h15_firmware *out)
{
	const struct ane_h15_fw *pin = firmware_for_soc(soc);
	const uint8_t *bytes = payload;
	uint8_t digest[ANE_SHA256_LEN];
	void *storage;

	if (!pin)
		return -ENODEV;
	if (!payload || !out || size != pin->size)
		return -EINVAL;
	if (out->storage)
		return -EBUSY;
	ane_sha256(payload, size, digest);
	if (memcmp(digest, pin->sha256, sizeof(digest)))
		return -EBADMSG;
	storage = calloc(1, H15_IMAGE_SIZE);
	if (!storage)
		return -ENOMEM;
	memcpy(storage, bytes + H15_TEXT_FILE, H15_TEXT_SIZE);
	memcpy((uint8_t *)storage + H15_TEXT_SIZE,
	       bytes + H15_DATA_FILE, H15_DATA_FILE_SIZE);
	out->storage = storage;
	out->size = H15_IMAGE_SIZE;
	out->entry = 0;
	return 0;
}

void ane_h15_firmware_free(struct ane_h15_firmware *image)
{
	if (!image)
		return;
	free(image->storage);
	memset(image, 0, sizeof(*image));
}
