// SPDX-License-Identifier: MIT
#ifndef LIBANE_H18_FIRMWARE_H
#define LIBANE_H18_FIRMWARE_H

#include <stdint.h>

struct ane_h18_firmware {
	void *storage;
	uint64_t size;
	uint64_t entry;
};

/* Exact 26A428 T8152 ANE0 payload only (h18_ane0_fw_kirkland_j8xx), engine 0.
 * Initialize out to zero; errors preserve it. Success owns the full Mach-O VM
 * layout, including zero-fill. This is host storage, not DMA residency or a
 * bootable image: no runtime patches, device access or publication occurs.
 * The exact whole-payload SHA-256 pins the layout; no parser runs. The caller
 * must retain a published owner until device retirement is proved. */
int ane_h18_firmware_stage(uint32_t soc, uint32_t engine, const void *payload,
			   uint64_t size, struct ane_h18_firmware *out);

/* Host storage only. Safe on zeroed or already-free owners. */
void ane_h18_firmware_free(struct ane_h18_firmware *image);

#endif
