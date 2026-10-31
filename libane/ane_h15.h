// SPDX-License-Identifier: MIT
#ifndef LIBANE_H15_H
#define LIBANE_H15_H

#include <stdint.h>

#define ANE_H15_LOAD_SECTIONS 6
#define ANE_H15_LOAD_COMMAND_BYTES 0x1c0

enum ane_h15_load_slot {
	ANE_H15_LOAD_GENERIC,
	ANE_H15_LOAD_CONSTANTS,
	ANE_H15_LOAD_TEXT,
	ANE_H15_LOAD_OPERATION,
	ANE_H15_LOAD_PROCEDURE,
	ANE_H15_LOAD_PROPERTY
};

struct ane_h15_payload {
	const void *data;
	uint64_t size;
};

/* Caller-declared residency, not proof of an IOMMU mapping. */
struct ane_h15_relocation {
	uint64_t iova;
	uint64_t capacity;
};

struct ane_h15_load {
	void *storage;
	const void *section[ANE_H15_LOAD_SECTIONS];
	uint8_t command[ANE_H15_LOAD_COMMAND_BYTES];
};

/* Fixed H15 add family only: payload sizes 664/16384/284/1040/56/40.
 * Inputs are already serialized; this function parses no HWX and validates no
 * firmware semantics. It owns one copy of each payload on success. Relocations
 * describe distinct 16-KiB-aligned ranges under the Linux allocation policy.
 * Initialize out to zero. Errors leave it unchanged; an existing owner is busy.
 * No device access, publication, firmware identity or completion is implied. */
int ane_h15_load_build(const struct ane_h15_payload payload[ANE_H15_LOAD_SECTIONS],
		      const struct ane_h15_relocation reloc[ANE_H15_LOAD_SECTIONS],
		      struct ane_h15_load *out);

/* Host storage only. After publication, the caller must retain this owner until
 * device ownership is proved released; an ambiguous completion is not release. */
void ane_h15_load_free(struct ane_h15_load *load);

#endif
