// SPDX-License-Identifier: MIT
#include "ane_h18_firmware.h"
#include "ane_sha256.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "../ane/h18/ane_h18_fw_pins.h"
#include "../ane/h18/ane_h18_fw_layout.h"

int ane_h18_firmware_stage(uint32_t soc, uint32_t engine, const void *payload,
			   uint64_t size, struct ane_h18_firmware *out)
{
	const struct ane_h18_fw *pin = &ane_fw_t8152_ane0;
	uint8_t digest[ANE_SHA256_LEN];
	void *storage;

	if (soc != 0x8152 || engine != 0)
		return -ENODEV;
	if (!payload || !out || size != pin->size)
		return -EINVAL;
	if (out->storage)
		return -EBUSY;
	ane_sha256(payload, size, digest);
	if (memcmp(digest, pin->sha256, sizeof(digest)))
		return -EBADMSG;
	storage = calloc(1, ANE_H18_FW_IMAGE_SIZE);
	if (!storage)
		return -ENOMEM;
	ane_h18_fw_copy(storage, payload);
	out->storage = storage;
	out->size = ANE_H18_FW_IMAGE_SIZE;
	out->entry = 0;
	return 0;
}

void ane_h18_firmware_free(struct ane_h18_firmware *image)
{
	if (!image)
		return;
	free(image->storage);
	memset(image, 0, sizeof(*image));
}
