// SPDX-License-Identifier: MIT
#include "ane_h15.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const uint64_t payload_sizes[ANE_H15_LOAD_SECTIONS] = {
	664, 16384, 284, 1040, 56, 40
};

static const unsigned int record_offsets[ANE_H15_LOAD_SECTIONS] = {
	0x8, 0x38, 0x68, 0x98, 0xc8, 0x128
};

static void put32(uint8_t *p, uint32_t value)
{
	unsigned int i;

	for (i = 0; i < 4; i++)
		p[i] = (uint8_t)(value >> (8 * i));
}

static void put64(uint8_t *p, uint64_t value)
{
	put32(p, (uint32_t)value);
	put32(p + 4, (uint32_t)(value >> 32));
}

static int validate(const struct ane_h15_payload *payload,
		    const struct ane_h15_relocation *reloc)
{
	unsigned int i, j;

	for (i = 0; i < ANE_H15_LOAD_SECTIONS; i++) {
		if (!payload[i].data || payload[i].size != payload_sizes[i] ||
		    !reloc[i].iova || (reloc[i].iova & 0x3fff) ||
		    (reloc[i].capacity & 0x3fff) ||
		    reloc[i].capacity < payload[i].size ||
		    reloc[i].iova > UINT64_MAX - reloc[i].capacity)
			return -EINVAL;
		for (j = 0; j < i; j++)
			if (reloc[i].iova < reloc[j].iova + reloc[j].capacity &&
			    reloc[j].iova < reloc[i].iova + reloc[i].capacity)
				return -EINVAL;
	}
	return 0;
}

int ane_h15_load_build(const struct ane_h15_payload payload[ANE_H15_LOAD_SECTIONS],
		      const struct ane_h15_relocation reloc[ANE_H15_LOAD_SECTIONS],
		      struct ane_h15_load *out)
{
	static const uint32_t ids[ANE_H15_LOAD_SECTIONS] = {
		1002, 4, 3, 1003, 1004, 1005
	};
	static const uint32_t types[ANE_H15_LOAD_SECTIONS] = { 4, 3, 4, 4, 4, 4 };
	uint8_t *storage, *p;
	size_t size = 0, offset = 0;
	unsigned int i;
	int err;

	if (!payload || !reloc || !out)
		return -EINVAL;
	if (out->storage)
		return -EBUSY;
	err = validate(payload, reloc);
	if (err)
		return err;
	for (i = 0; i < ANE_H15_LOAD_SECTIONS; i++)
		size += (size_t)((payload_sizes[i] + 7) & ~7ULL);
	storage = calloc(1, size);
	if (!storage)
		return -ENOMEM;
	memset(out, 0, sizeof(*out));
	out->storage = storage;
	put32(out->command + 4, 0x200);
	for (i = 0; i < ANE_H15_LOAD_SECTIONS; i++) {
		out->section[i] = storage + offset;
		memcpy(storage + offset, payload[i].data, (size_t)payload[i].size);
		offset += (size_t)((payload[i].size + 7) & ~7ULL);
		p = out->command + record_offsets[i];
		put32(p, 1);
		put32(p + 4, ids[i]);
		put32(p + 8, types[i]);
		put32(p + 16, 1);
		put64(p + 24, reloc[i].iova);
		put64(p + 32, payload[i].size);
	}
	return 0;
}

void ane_h15_load_free(struct ane_h15_load *load)
{
	if (!load)
		return;
	free(load->storage);
	memset(load, 0, sizeof(*load));
}
