// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

#include <drm.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ane_v3.h"

#ifndef LIBANE_CONFIG_NO_ERR
#define ane_v3_err(a, ...) fprintf(stderr, "LIBANE: V3: ERR: " a, ##__VA_ARGS__)
#else
#define ane_v3_err(...) \
	do {            \
	} while (0)
#endif

#define ANE_V3_HWX_MAGIC		0xbeeffaceu
#define ANE_V3_HWX_CPUTYPE		0x80u
#define ANE_V3_HWX_MAX_BYTES		(64u * 1024u * 1024u)
#define ANE_V3_HWX_MAX_CMDS		64u
#define ANE_V3_HWX_PROG_MIN		0x890u

#define ANE_V3_GENERIC_MAGIC		1u
#define ANE_V3_GENERIC_VERSION		0x10u
#define ANE_V3_GENERIC_HEADER		0x208u
#define ANE_V3_GENERIC_ENTRY		0x30u
#define ANE_V3_GENERIC_ENTRY_TAIL	0xffffu
#define ANE_V3_FIRST_OUTPUT_ID		4u
#define ANE_V3_MAX_BUFFER_ID		0x3du

#define ANE_V3_OPERATION_RECORD		0x40cu
#define ANE_V3_OPERATION_REFS_MAX	((ANE_V3_OPERATION_RECORD - 0x0cu) / 8u)
#define ANE_V3_PROCEDURE_SIZE		0x38u
#define ANE_V3_PROPERTIES_SIZE		0x28u
#define ANE_V3_DMA_SLOTS		64u

static uint32_t rd_u32(const uint8_t *p, size_t off)
{
	uint32_t v;

	memcpy(&v, p + off, sizeof(v));
	return v;
}

static uint64_t rd_u64(const uint8_t *p, size_t off)
{
	uint64_t v;

	memcpy(&v, p + off, sizeof(v));
	return v;
}

static void wr_u32(uint8_t *p, size_t off, uint32_t v)
{
	memcpy(p + off, &v, sizeof(v));
}

static void wr_u64(uint8_t *p, size_t off, uint64_t v)
{
	memcpy(p + off, &v, sizeof(v));
}

static int fits(size_t start, size_t len, size_t total)
{
	return start <= total && len <= total - start;
}

static int name_eq(const uint8_t *field, const char *want)
{
	size_t n = strlen(want);
	size_t i;

	if (n >= 16)
		return 0;
	if (memcmp(field, want, n) != 0)
		return 0;
	for (i = n; i < 16; i++) {
		if (field[i] != 0)
			return 0;
	}
	return 1;
}

static int zero_bytes(const uint8_t *p, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (p[i] != 0)
			return 0;
	}
	return 1;
}

static uint32_t popcount16(uint32_t mask)
{
	uint32_t c = 0;

	mask &= 0xffffu;
	while (mask) {
		c += mask & 1u;
		mask >>= 1;
	}
	return c;
}

/* Valid byte ranges of the H15/H16/H17 register image, exclusive ends. */
static int register_valid(uint32_t word)
{
	static const struct {
		uint32_t start;
		uint32_t end;
	} blocks[] = {
		{ 0x0000u, 0x005cu }, { 0x4100u, 0x41a4u }, { 0x44d0u, 0x44d4u },
		{ 0x4500u, 0x453cu }, { 0x4900u, 0x4930u }, { 0x4d00u, 0x4e44u },
		{ 0x5100u, 0x5154u }, { 0x5500u, 0x5620u }, { 0x5900u, 0x5930u },
	};
	uint32_t addr = word * 4u;
	size_t i;

	for (i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
		if (addr >= blocks[i].start && addr < blocks[i].end)
			return 1;
	}
	return 0;
}

/* Walk an H15 (8-word header) or H16/H17 (9-word header) task stream. */
static int walk_task_stream(const uint8_t *stream, size_t size,
			    uint32_t header_words, int allow_zero_prologue,
			    uint32_t *tasks_out, uint64_t *slots_out)
{
	uint32_t prologue;
	uint32_t tasks = 0;
	uint64_t slots = 0;
	size_t pos = 16;

	if (size < 16 || (size & 3u) != 0)
		return -ENOEXEC;
	prologue = rd_u32(stream, 0);
	if (prologue != 1u && !(allow_zero_prologue && prologue == 0u))
		return -ENOEXEC;
	if (!zero_bytes(stream + 4, 12))
		return -ENOEXEC;

	while (pos < size) {
		uint32_t words;
		uint32_t hdr = header_words;
		size_t end;
		size_t rec;
		size_t next;

		if (size - pos < 16)
			return -ENOEXEC;
		words = (rd_u32(stream, pos) >> 16) & 0x7ffu;
		if (!words) {
			if (!zero_bytes(stream + pos, 16))
				return -ENOEXEC;
			pos += 16;
			continue;
		}
		if (words < hdr || words > (size - pos) / 4u)
			return -ENOEXEC;
		end = pos + (size_t)words * 4u;
		if ((rd_u32(stream, pos + (size_t)(hdr - 1u) * 4u) & 3u) == 3u)
			hdr++;
		if (words < hdr)
			return -ENOEXEC;

		rec = pos + (size_t)hdr * 4u;
		while (rec < end) {
			uint32_t h = rd_u32(stream, rec);
			uint32_t base = h & 0x7fffu;
			uint32_t count;
			uint32_t i;

			rec += 4;
			if (h & (1u << 31)) {
				uint32_t mask = (h >> 15) & 0xffffu;

				count = 1u + popcount16(mask);
				if (count > (end - rec) / 4u || !register_valid(base))
					return -ENOEXEC;
				for (i = 0; i < 16; i++) {
					if ((mask & (1u << i)) &&
					    !register_valid(base + i + 1u))
						return -ENOEXEC;
				}
			} else {
				count = ((h >> 15) & 0x3fu) + 1u;
				if (count > (end - rec) / 4u || !register_valid(base))
					return -ENOEXEC;
				for (i = 1; i < count; i++) {
					if (!register_valid(base + i))
						return -ENOEXEC;
				}
				if (h & (1u << 29)) {
					uint32_t slot = (h >> 23) & 0x3fu;

					slots |= 1ULL << slot;
				}
			}
			rec += (size_t)count * 4u;
		}

		next = (end == size) ? size : ((end + 15u) & ~(size_t)15u);
		if (next > size || !zero_bytes(stream + end, next - end))
			return -ENOEXEC;
		pos = next;
		tasks++;
	}

	if (!tasks)
		return -ENOEXEC;
	*tasks_out = tasks;
	*slots_out = slots;
	return 0;
}

void ane_v3_sections_free(struct ane_v3_sections *secs)
{
	unsigned int i;

	if (!secs)
		return;
	for (i = 0; i < DRM_ANE_H16_SECTIONS; i++) {
		free(secs->data[i]);
		secs->data[i] = NULL;
		secs->size[i] = 0;
	}
}

int ane_v3_build_sections(const void *hwx, size_t hwx_size,
			  struct ane_v3_sections *out)
{
	const uint8_t *b = (const uint8_t *)hwx;
	uint32_t subtype;
	uint32_t header_words;
	int allow_zero_prologue;
	uint32_t ncmds;
	uint32_t sizeofcmds;
	size_t cmd_end;
	size_t pos = 32;
	uint32_t i;

	uint64_t surf_vm[DRM_ANE_H16_SURFACES];
	uint64_t surf_alloc[DRM_ANE_H16_SURFACES];
	uint32_t surf_dir[DRM_ANE_H16_SURFACES];
	uint32_t surf_id[DRM_ANE_H16_SURFACES];
	uint32_t surf_count = 0;
	uint32_t output_count = 0;

	int have_text = 0;
	uint64_t text_size = 0;
	uint32_t text_off = 0;

	int have_kernel = 0;
	uint64_t kernel_vm = 0;

	int have_scratch = 0;
	uint64_t scratch_vm = 0;
	uint64_t scratch_size = 0;

	int have_desc = 0;
	size_t desc_off = 0;
	uint32_t desc_len = 0;

	uint32_t tasks = 0;
	uint64_t slots = 0;
	uint32_t next_in_id = ANE_V3_FIRST_OUTPUT_ID + 1u;
	uint32_t scratch_id = 0;
	uint32_t entries;
	uint32_t generic_size;
	uint8_t *generic = NULL;
	uint8_t *operation = NULL;
	uint8_t *procedure = NULL;
	uint8_t *properties = NULL;
	uint32_t ref_count = 0;
	uint32_t slot;
	int err;

	static const uint32_t proc_call[8] = {
		1u, 3u, 0u, 0u, 1u, 0u, 0xffffffffu, 4u
	};

	if (!hwx || !out || hwx_size < 32 || hwx_size > ANE_V3_HWX_MAX_BYTES)
		return -EINVAL;
	memset(out, 0, sizeof(*out));

	if (rd_u32(b, 0) != ANE_V3_HWX_MAGIC ||
	    rd_u32(b, 4) != ANE_V3_HWX_CPUTYPE) {
		ane_v3_err("not an HWX container\n");
		return -ENOEXEC;
	}

	subtype = rd_u32(b, 8);
	if (subtype == ANE_V3_HWX_SUBTYPE_H15) {
		header_words = 8u;
		allow_zero_prologue = 1;
	} else if (subtype == ANE_V3_HWX_SUBTYPE_H16) {
		header_words = 9u;
		allow_zero_prologue = 0;
	} else if (subtype == ANE_V3_HWX_SUBTYPE_H17) {
		header_words = 9u;
		allow_zero_prologue = 1;
	} else {
		ane_v3_err("unsupported HWX subtype %u (expected 6/7/9)\n",
			   subtype);
		return -ENOEXEC;
	}

	ncmds = rd_u32(b, 16);
	sizeofcmds = rd_u32(b, 20);
	if (!ncmds || ncmds > ANE_V3_HWX_MAX_CMDS ||
	    !fits(32, sizeofcmds, hwx_size)) {
		ane_v3_err("invalid HWX load command table\n");
		return -ENOEXEC;
	}
	cmd_end = 32u + (size_t)sizeofcmds;

	for (i = 0; i < ncmds; i++) {
		uint32_t cmd;
		uint32_t len;

		if (!fits(pos, 8, cmd_end))
			return -ENOEXEC;
		cmd = rd_u32(b, pos);
		len = rd_u32(b, pos + 4);
		if (len < 8 || (len & 7u) != 0 || !fits(pos, len, cmd_end))
			return -ENOEXEC;

		if (cmd == 0x19u) {
			uint64_t vmsize;
			uint32_t prot;
			uint32_t nsects;
			uint32_t j;
			const uint8_t *seg = b + pos + 8;

			if (len < 72u)
				return -ENOEXEC;
			vmsize = rd_u64(b, pos + 32);
			prot = rd_u32(b, pos + 60);
			nsects = rd_u32(b, pos + 64);
			if (nsects > 16u || len != 72u + nsects * 80u)
				return -ENOEXEC;

			for (j = 0; j < nsects; j++) {
				size_t at = pos + 72u + (size_t)j * 80u;
				uint64_t svm = rd_u64(b, at + 32);
				uint64_t ssz = rd_u64(b, at + 40);
				uint32_t soff = rd_u32(b, at + 48);

				if (name_eq(seg, "__FVMLIB")) {
					if ((prot != 1u && prot != 2u) ||
					    surf_count >= DRM_ANE_H16_SURFACES ||
					    !vmsize)
						return -ENOEXEC;
					surf_vm[surf_count] = svm;
					surf_alloc[surf_count] = vmsize;
					surf_dir[surf_count] = prot;
					if (prot == 2u)
						output_count++;
					surf_count++;
				} else if (name_eq(seg, "__TEXT") &&
					   name_eq(b + at, "__text")) {
					if (have_text || !ssz)
						return -ENOEXEC;
					have_text = 1;
					text_size = ssz;
					text_off = soff;
				} else if (name_eq(seg, "__KERN_0") &&
					   name_eq(b + at, "__kern_0")) {
					if (have_kernel || !ssz)
						return -ENOEXEC;
					have_kernel = 1;
					kernel_vm = svm;
				} else if (name_eq(seg, "__DATA") &&
					   name_eq(b + at, "__bss")) {
					if (have_scratch || prot != 3u || !ssz)
						return -ENOEXEC;
					have_scratch = 1;
					scratch_vm = svm;
					scratch_size = ssz;
				}
			}
		} else if (cmd == 4u) {
			if (len < 16u)
				return -ENOEXEC;
			if (rd_u32(b, pos + 8) == 4u) {
				if (have_desc || len < ANE_V3_HWX_PROG_MIN)
					return -ENOEXEC;
				have_desc = 1;
				desc_off = pos;
				desc_len = len;
			}
		}
		pos += len;
	}

	if (pos != cmd_end || !have_text || !have_desc || surf_count < 2u ||
	    output_count != 1u) {
		ane_v3_err("HWX missing required text/descriptor/surfaces\n");
		return -ENOEXEC;
	}
	if (text_size > hwx_size || !fits(text_off, (size_t)text_size, hwx_size)) {
		ane_v3_err("HWX task stream outside file bounds\n");
		return -ENOEXEC;
	}

	err = walk_task_stream(b + text_off, (size_t)text_size, header_words,
			       allow_zero_prologue, &tasks, &slots);
	if (err < 0) {
		ane_v3_err("HWX task stream parse failed (%d)\n", err);
		return err;
	}

	/* Assign buffer IDs: output = 4, inputs = 5, 6, ..., optional scratch last. */
	for (i = 0; i < surf_count; i++) {
		if (surf_dir[i] == 2u) {
			surf_id[i] = ANE_V3_FIRST_OUTPUT_ID;
		} else {
			if (next_in_id >= ANE_V3_MAX_BUFFER_ID)
				return -ENOEXEC;
			surf_id[i] = next_in_id++;
		}
	}
	if (have_scratch) {
		if (next_in_id >= ANE_V3_MAX_BUFFER_ID)
			return -ENOEXEC;
		scratch_id = next_in_id++;
	}

	entries = surf_count + (have_scratch ? 1u : 0u);
	generic_size = ANE_V3_GENERIC_HEADER + ANE_V3_GENERIC_ENTRY * entries;
	if (generic_size < DRM_ANE_H16_SECTION_MIN ||
	    generic_size > DRM_ANE_H16_SECTION_MAX)
		return -ENOEXEC;

	generic = calloc(1, generic_size);
	operation = calloc(1, 4u + ANE_V3_OPERATION_RECORD);
	procedure = calloc(1, ANE_V3_PROCEDURE_SIZE);
	properties = calloc(1, ANE_V3_PROPERTIES_SIZE);
	if (!generic || !operation || !procedure || !properties) {
		err = -ENOMEM;
		goto fail;
	}

	/* 1. Generic section */
	wr_u32(generic, 0x00u, ANE_V3_GENERIC_MAGIC);
	wr_u32(generic, 0x04u, ANE_V3_GENERIC_VERSION);
	wr_u32(generic, 0x204u, entries);
	for (i = 0; i < entries; i++) {
		size_t e = ANE_V3_GENERIC_HEADER + (size_t)i * ANE_V3_GENERIC_ENTRY;
		uint32_t id = (i < surf_count) ? surf_id[i] : scratch_id;
		uint32_t dir = (i < surf_count) ? surf_dir[i] : 1u;
		uint64_t sz = (i < surf_count) ? surf_alloc[i] : scratch_size;

		wr_u32(generic, e + 0x00u, 1u);
		wr_u32(generic, e + 0x04u, id);
		wr_u32(generic, e + 0x08u, dir - 1u);
		wr_u32(generic, e + 0x10u, dir);
		wr_u64(generic, e + 0x20u, sz);
		wr_u32(generic, e + 0x28u, ANE_V3_GENERIC_ENTRY_TAIL);
	}

	/* 2. Operation section: bind every nonzero BAR slot referenced in text */
	wr_u32(operation, 0x00u, 1u);
	for (slot = 0; slot < ANE_V3_DMA_SLOTS; slot++) {
		size_t boff;
		uint64_t base;
		uint32_t tag = 0;
		int matched = 0;

		if (!(slots & (1ULL << slot)))
			continue;
		boff = (size_t)0x10u * (size_t)(slot + 1u);
		if (boff + 8u > desc_len) {
			err = -ENOEXEC;
			goto fail;
		}
		base = rd_u64(b, desc_off + boff);
		if (!base)
			continue;
		for (i = 0; i < surf_count; i++) {
			if (surf_vm[i] == base) {
				tag = surf_id[i];
				matched = 1;
				break;
			}
		}
		if (!matched && have_scratch && base == scratch_vm) {
			tag = scratch_id;
			matched = 1;
		}
		if (!matched && have_kernel && base == kernel_vm) {
			tag = DRM_ANE_H16_RESOURCE_KERNEL;
			matched = 1;
		}
		if (!matched) {
			ane_v3_err("BAR slot %u base 0x%llx names no surface, "
				   "scratch or kernel\n",
				   slot, (unsigned long long)base);
			err = -ENOEXEC;
			goto fail;
		}
		if (ref_count >= ANE_V3_OPERATION_REFS_MAX) {
			err = -ENOEXEC;
			goto fail;
		}
		wr_u32(operation, 4u + 0x0cu + (size_t)ref_count * 8u, slot);
		wr_u32(operation, 4u + 0x10u + (size_t)ref_count * 8u, tag);
		ref_count++;
	}
	if (!ref_count) {
		ane_v3_err("task stream binds no buffer through a BAR slot\n");
		err = -ENOEXEC;
		goto fail;
	}
	wr_u32(operation, 4u + 0x08u, ref_count);

	/* 3. Procedure section (fixed single-procedure form) */
	wr_u32(procedure, 0x00u, 1u);
	wr_u64(procedure, 0x08u, 0x18u);
	wr_u64(procedure, 0x10u, sizeof(proc_call));
	for (i = 0; i < 8u; i++)
		wr_u32(procedure, 0x18u + (size_t)i * 4u, proc_call[i]);

	/* 4. Text properties section */
	wr_u32(properties, 0x00u, 1u);
	wr_u32(properties, 0x0cu, tasks);
	wr_u64(properties, 0x20u, text_size);

	out->data[0] = generic;
	out->size[0] = generic_size;
	out->data[1] = operation;
	out->size[1] = 4u + ANE_V3_OPERATION_RECORD;
	out->data[2] = procedure;
	out->size[2] = ANE_V3_PROCEDURE_SIZE;
	out->data[3] = properties;
	out->size[3] = ANE_V3_PROPERTIES_SIZE;
	out->subtype = subtype;
	out->tasks = tasks;
	out->surface_count = surf_count;
	for (i = 0; i < surf_count; i++) {
		out->surface_direction[i] = surf_dir[i];
		out->surface_size[i] = surf_alloc[i];
		out->surface_id[i] = surf_id[i];
	}
	out->has_scratch = have_scratch;
	out->scratch_id = scratch_id;
	out->scratch_size = scratch_size;
	return 0;

fail:
	free(generic);
	free(operation);
	free(procedure);
	free(properties);
	return err;
}

struct ane_v3_ctx {
	uint32_t surface_count;
	uint32_t src_count;
	uint32_t dst_count;
	uint32_t surface_direction[DRM_ANE_H16_SURFACES];
	uint64_t surface_size[DRM_ANE_H16_SURFACES];
	uint32_t src_surface[DRM_ANE_H16_SURFACES];
	uint32_t dst_surface[DRM_ANE_H16_SURFACES];
	void *surface_map[DRM_ANE_H16_SURFACES];
	void *surface_buf[DRM_ANE_H16_SURFACES];
	int in_place;
};

static int read_whole_file(const char *path, void **buf_out, size_t *size_out)
{
	FILE *fp;
	long end;
	void *buf;

	fp = fopen(path, "rb");
	if (!fp) {
		ane_v3_err("failed to open %s: %s\n", path, strerror(errno));
		return -errno;
	}
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return -EINVAL;
	}
	end = ftell(fp);
	if (end <= 0 || (uint64_t)end > ANE_V3_HWX_MAX_BYTES ||
	    fseek(fp, 0, SEEK_SET) != 0) {
		ane_v3_err("invalid HWX size on %s\n", path);
		fclose(fp);
		return -EINVAL;
	}
	buf = malloc((size_t)end);
	if (!buf) {
		fclose(fp);
		return -ENOMEM;
	}
	if (fread(buf, 1, (size_t)end, fp) != (size_t)end) {
		ane_v3_err("short read on %s\n", path);
		free(buf);
		fclose(fp);
		return -EIO;
	}
	fclose(fp);
	*buf_out = buf;
	*size_out = (size_t)end;
	return 0;
}

static void unmap_surfaces(struct ane_v3_ctx *ctx)
{
	uint32_t i;

	for (i = 0; i < ctx->surface_count; i++) {
		if (ctx->surface_map[i]) {
			munmap(ctx->surface_map[i], (size_t)ctx->surface_size[i]);
			ctx->surface_map[i] = NULL;
		}
	}
}

static void free_buffers(struct ane_v3_ctx *ctx)
{
	uint32_t i;

	for (i = 0; i < ctx->surface_count; i++) {
		free(ctx->surface_buf[i]);
		ctx->surface_buf[i] = NULL;
	}
}

int ane_v3_open(struct ane_nn *nn, const char *path, int abi_minor)
{
	void *hwx = NULL;
	size_t hwx_size = 0;
	struct ane_v3_sections secs;
	struct drm_ane_h16_section ksecs[DRM_ANE_H16_SECTIONS];
	struct drm_ane_h16_load load;
	struct ane_v3_ctx *ctx = NULL;
	uint32_t i;
	int err;

	memset(&secs, 0, sizeof(secs));
	memset(ksecs, 0, sizeof(ksecs));
	memset(&load, 0, sizeof(load));

	err = read_whole_file(path, &hwx, &hwx_size);
	if (err < 0)
		return err;

	err = ane_v3_build_sections(hwx, hwx_size, &secs);
	if (err < 0) {
		free(hwx);
		return err;
	}

	for (i = 0; i < DRM_ANE_H16_SECTIONS; i++) {
		ksecs[i].kind = DRM_ANE_H16_SECTION_GENERIC + i;
		ksecs[i].size = secs.size[i];
		ksecs[i].data_ptr = (uint64_t)(uintptr_t)secs.data[i];
	}
	load.hwx_ptr = (uint64_t)(uintptr_t)hwx;
	load.hwx_size = (uint64_t)hwx_size;
	load.sections_ptr = (uint64_t)(uintptr_t)ksecs;
	load.section_count = DRM_ANE_H16_SECTIONS;

	if (ioctl(nn->fd, DRM_IOCTL_ANE_H16_LOAD, &load) < 0) {
		err = -errno;
		ane_v3_err("DRM_IOCTL_ANE_H16_LOAD failed on %s: %s\n",
			   path, strerror(-err));
		ane_v3_sections_free(&secs);
		free(hwx);
		return err;
	}

	ane_v3_sections_free(&secs);
	free(hwx);

	if (load.surface_count < 2u || load.surface_count > DRM_ANE_H16_SURFACES) {
		ane_v3_err("unexpected surface_count %u from H16_LOAD\n",
			   load.surface_count);
		return -EINVAL;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return -ENOMEM;

	ctx->surface_count = load.surface_count;
	for (i = 0; i < load.surface_count; i++) {
		ctx->surface_size[i] = load.surface_size[i];
		ctx->surface_direction[i] = load.surface_direction[i];
		if (!load.surface_size[i]) {
			free(ctx);
			return -EINVAL;
		}
		if (load.surface_direction[i] == 1u) {
			ctx->src_surface[ctx->src_count++] = i;
		} else if (load.surface_direction[i] == 2u) {
			ctx->dst_surface[ctx->dst_count++] = i;
		} else {
			free(ctx);
			return -EINVAL;
		}
	}

	/* Prefer zero-copy mmap + EXEC_IN_PLACE on ABI 3 minor >= 1; fall back
	 * cleanly to copy-mode buffers when minor == 0 or mmap is unavailable. */
	if (abi_minor >= 1 && sizeof(off_t) >= 8) {
		int all_mapped = 1;

		for (i = 0; i < ctx->surface_count; i++) {
			int prot = (ctx->surface_direction[i] == 1u)
				? (PROT_READ | PROT_WRITE)
				: PROT_READ;
			void *p = mmap(NULL, (size_t)ctx->surface_size[i], prot,
				       MAP_SHARED, nn->fd,
				       (off_t)DRM_ANE_H16_MMAP_OFFSET(i));

			if (p == MAP_FAILED) {
				all_mapped = 0;
				break;
			}
			ctx->surface_map[i] = p;
		}
		if (all_mapped) {
			ctx->in_place = 1;
		} else {
			unmap_surfaces(ctx);
		}
	}

	if (!ctx->in_place) {
		for (i = 0; i < ctx->surface_count; i++) {
			ctx->surface_buf[i] = calloc(1, (size_t)ctx->surface_size[i]);
			if (!ctx->surface_buf[i]) {
				free_buffers(ctx);
				free(ctx);
				return -ENOMEM;
			}
		}
	}

	/* Populate nn->anec counts so ane_src_count/ane_dst_count macros work. */
	{
		uint32_t sc = ctx->src_count;
		uint32_t dc = ctx->dst_count;

		memcpy((uint8_t *)&nn->anec + offsetof(struct anec, src_count),
		       &sc, sizeof(sc));
		memcpy((uint8_t *)&nn->anec + offsetof(struct anec, dst_count),
		       &dc, sizeof(dc));
	}

	nn->v3 = ctx;
	return 0;
}

void ane_v3_close(struct ane_nn *nn)
{
	struct ane_v3_ctx *ctx;

	if (!nn || !nn->v3)
		return;
	ctx = (struct ane_v3_ctx *)nn->v3;
	unmap_surfaces(ctx);
	free_buffers(ctx);
	free(ctx);
	nn->v3 = NULL;
}

int ane_v3_exec(struct ane_nn *nn)
{
	struct ane_v3_ctx *ctx = (struct ane_v3_ctx *)nn->v3;
	struct drm_ane_h16_buffer bufs[DRM_ANE_H16_SURFACES];
	struct drm_ane_h16_exec exec;
	uint32_t i;

	memset(&exec, 0, sizeof(exec));
	exec.count = ctx->surface_count;

	if (ctx->in_place) {
		exec.buffers_ptr = 0;
		exec.flags = DRM_ANE_H16_EXEC_IN_PLACE;
	} else {
		memset(bufs, 0, sizeof(bufs));
		for (i = 0; i < ctx->surface_count; i++) {
			bufs[i].ptr = (uint64_t)(uintptr_t)ctx->surface_buf[i];
			bufs[i].size = ctx->surface_size[i];
		}
		exec.buffers_ptr = (uint64_t)(uintptr_t)bufs;
		exec.flags = 0;
	}

	if (ioctl(nn->fd, DRM_IOCTL_ANE_H16_EXEC, &exec) < 0)
		return -errno;
	return 0;
}

uint64_t ane_v3_src_size(struct ane_nn *nn, uint32_t idx)
{
	struct ane_v3_ctx *ctx = (struct ane_v3_ctx *)nn->v3;

	if (!ctx || idx >= ctx->src_count)
		return 0;
	return ctx->surface_size[ctx->src_surface[idx]];
}

uint64_t ane_v3_dst_size(struct ane_nn *nn, uint32_t idx)
{
	struct ane_v3_ctx *ctx = (struct ane_v3_ctx *)nn->v3;

	if (!ctx || idx >= ctx->dst_count)
		return 0;
	return ctx->surface_size[ctx->dst_surface[idx]];
}

void ane_v3_send(struct ane_nn *nn, const void *from, uint32_t idx)
{
	struct ane_v3_ctx *ctx = (struct ane_v3_ctx *)nn->v3;
	uint32_t s;
	void *dst;

	if (!ctx || idx >= ctx->src_count)
		return;
	s = ctx->src_surface[idx];
	dst = ctx->in_place ? ctx->surface_map[s] : ctx->surface_buf[s];
	memcpy(dst, from, (size_t)ctx->surface_size[s]);
}

void ane_v3_read(struct ane_nn *nn, void *to, uint32_t idx)
{
	struct ane_v3_ctx *ctx = (struct ane_v3_ctx *)nn->v3;
	uint32_t s;
	const void *src;

	if (!ctx || idx >= ctx->dst_count)
		return;
	s = ctx->dst_surface[idx];
	src = ctx->in_place ? ctx->surface_map[s] : ctx->surface_buf[s];
	memcpy(to, src, (size_t)ctx->surface_size[s]);
}
