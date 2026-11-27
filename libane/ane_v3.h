// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

#ifndef __ANE_V3_H__
#define __ANE_V3_H__

#include <stddef.h>
#include <stdint.h>

#include <asm/types.h>
#include <ane_accel.h>
#include "ane.h"

/*
 * ABI-3 backend for the in-tree aurora ane_t6021 accel node on H15 (T8122 /
 * base M3), H16 and H17. Parses an HWX container, builds the four host
 * sections (generic, operation, procedure, text_properties), submits
 * DRM_IOCTL_ANE_H16_LOAD, and runs DRM_IOCTL_ANE_H16_EXEC using mmap in-place
 * surfaces when the node reports DRM_IOCTL_VERSION minor >= 1 (falling back to
 * copy-mode buffers when minor == 0 or mmap is unsupported).
 */

#define ANE_V3_HWX_SUBTYPE_H15	6u
#define ANE_V3_HWX_SUBTYPE_H16	7u
#define ANE_V3_HWX_SUBTYPE_H17	9u

struct ane_v3_sections {
	void *data[DRM_ANE_H16_SECTIONS];
	uint32_t size[DRM_ANE_H16_SECTIONS];
	uint32_t subtype;
	uint32_t tasks;
	uint32_t surface_count;
	uint32_t surface_direction[DRM_ANE_H16_SURFACES];
	uint64_t surface_size[DRM_ANE_H16_SURFACES];
	uint32_t surface_id[DRM_ANE_H16_SURFACES];
	int has_scratch;
	uint32_t scratch_id;
	uint64_t scratch_size;
};

/* Parse an H15/H16/H17 HWX buffer and build the four ABI-3 LOAD sections. */
int ane_v3_build_sections(const void *hwx, size_t hwx_size,
			  struct ane_v3_sections *out);
void ane_v3_sections_free(struct ane_v3_sections *secs);

/* Runtime hooks dispatched by libane/ane.c when DRM_IOCTL_VERSION major == 3. */
int ane_v3_open(struct ane_nn *nn, const char *path, int abi_minor);
void ane_v3_close(struct ane_nn *nn);
int ane_v3_exec(struct ane_nn *nn);
uint64_t ane_v3_src_size(struct ane_nn *nn, uint32_t idx);
uint64_t ane_v3_dst_size(struct ane_nn *nn, uint32_t idx);
void ane_v3_send(struct ane_nn *nn, const void *from, uint32_t idx);
void ane_v3_read(struct ane_nn *nn, void *to, uint32_t idx);

#endif /* __ANE_V3_H__ */
