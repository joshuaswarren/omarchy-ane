// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

#include <asm/types.h>
#include <drm.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <ane_accel.h>
#include "ane.h"
#include "ane_m2.h"

#define ANEC_M2_HEADER_SIZE 0x1000UL
#define TASK_OFFSET	   0x1000UL
#define FRAME_BYTES	   16UL
/* Task stream region: 0x1000..0x1140, then the 0x4000-byte constant region.
 * Fixed offsets of the H14 anec layout (encodeANEC); proven on the fixture. */
#define CONST_OFFSET	 0x1140UL
#define ANE_M2_KRN_BYTES 0x4000UL

const uint32_t ane_m2_section_ids[ANE_M2_SEC_COUNT] = {1, 2, 3, 4, 5, 7};

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	       (uint32_t)p[3] << 24;
}

static uint64_t le64(const uint8_t *p)
{
	return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32;
}

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)((uint32_t)p[0] | (uint32_t)p[1] << 8);
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

static void put_le64(uint8_t *p, uint64_t v)
{
	put_le32(p, (uint32_t)v);
	put_le32(p + 4, (uint32_t)(v >> 32));
}

static void *sec_alloc(struct ane_m2_sections *secs, int which, uint64_t size)
{
	secs->sec[which].data = calloc(1, size);
	secs->sec[which].size = size;
	return secs->sec[which].data;
}

static int fail(const char *why)
{
	ane_m2_err("%s; refusing to build (outside the proven envelope)\n",
		   why);
	return -EINVAL;
}

/*
 * Descriptor address records (the only hardware-proven decode):
 *   u32 header with bit29 = BAR reference, bits 28:23 = local BAR slot,
 *   followed by an 8-byte IOVA placeholder (zero in the anec; the firmware
 *   patches it from BAR[slot] at load, pushToHWDirect fw135 0x44c98).
 * PROVEN: slots 4,5,6 carried a,y,b bit-exact through the scratch-module
 *   sequencer run.
 * INFERRED (one program): the records appear in task order inputs-first,
 *   outputs-last, and record order matches ascending input channel order
 *   (a=5, b=6) with the output channel 4 last. The anec carries no
 *   direction field this decode can read, so the builder accepts the
 *   association only when it is fully consistent (every io buffer bound
 *   exactly once, distinct slots) and refuses everything else.
 */
struct bar_rec {
	uint32_t slot;
};

static int walk_bar_records(const uint8_t *desc, uint64_t bytes,
			    const struct ane_m2_model *m,
			    struct ane_m2_ref *refs, uint32_t *ref_count)
{
	struct bar_rec rec[ANE_M2_MAX_BINDS];
	uint32_t n = 0;
	uint32_t i;
	uint32_t j;
	uint32_t in = 0;
	uint64_t off;

	for (off = FRAME_BYTES; off + 12 <= bytes; off += 4) {
		uint32_t w = le32(desc + off);
		uint32_t slot;

		if (!((w >> 29) & 1)) {
			continue;
		}
		/* A record candidate is only a record when its IOVA
		 * placeholder is still zero (the firmware patches it at
		 * load); task data words with bit29 set carry other data
		 * there (the fixture has 0xb60e0000 at +0x38). */
		if (le64(desc + off + 4)) {
			continue;
		}
		slot = (w >> 23) & 0x3f;
		if (!slot || slot >= TILE_COUNT) {
			return fail("address record slot out of [1,0x1f]");
		}
		if (n == ANE_M2_MAX_BINDS) {
			return fail("too many BAR address records");
		}
		rec[n].slot = slot;
		n++;
	}

	if (n != m->io_count) {
		return fail("BAR record count does not match the io table");
	}

	/* Assign input channels in record order, then the output. */
	for (i = 0; i < n; i++) {
		if (in < m->io_count - 1 && m->io[in].dir == 0) {
			refs[i].tag = m->io[in].buffer_id;
			in++;
		} else {
			refs[i].tag = m->io[m->io_count - 1].buffer_id;
		}
		refs[i].slot = rec[i].slot;
	}

	/* Emission order: slot ascending (the proven order 4,5,6). */
	for (i = 0; i < n; i++) {
		for (j = i + 1; j < n; j++) {
			if (refs[j].slot < refs[i].slot) {
				struct ane_m2_ref t = refs[i];
				refs[i] = refs[j];
				refs[j] = t;
			}
		}
	}
	for (i = 1; i < n; i++) {
		if (refs[i].slot == refs[i - 1].slot) {
			return fail("duplicate BAR slot in address records");
		}
	}

	/* Every io buffer must be bound exactly once. */
	for (i = 0; i < m->io_count; i++) {
		uint32_t hits = 0;

		for (j = 0; j < n; j++) {
			if (refs[j].tag == m->io[i].buffer_id) {
				hits++;
			}
		}
		if (hits != 1) {
			return fail("io buffer not covered by exactly one ref");
		}
	}

	*ref_count = n;
	return 0;
}

/*
 * tdprop deep walk, re-derived from the firmware checker (fw135 0x486a0):
 * stride is 0x30 when the first descriptor word has bit2 set, else 0x10;
 * each block's size is ((u16@[blk+2] & 0x7ff) << 2 + 0xf) & 0x3ff0; a zero
 * block word fails; when the first word has bit0 set, u32@[last+0x18] must
 * be 0. The segment's blockNbr is the walked block count (proven == 1 on
 * the fixture, where the Python builder hardcoded it).
 */
static int tdprop_block_count(const uint8_t *desc, uint64_t size)
{
	uint32_t w0 = le32(desc);
	uint64_t stride = (w0 & 4) ? 0x30 : 0x10;
	uint64_t cur = stride;
	uint64_t last = 0;
	int walked = 0;

	while (size > cur) {
		uint32_t blk = le16(desc + cur + 2) & 0x7ff;

		last = cur;
		if (!blk) {
			return fail("tdprop walk hit a zero block word");
		}
		cur += ((blk << 2) + 0xf) & 0x3ff0;
		walked++;
	}
	if ((w0 & 1) && size > stride && le32(desc + last + 0x18)) {
		return fail("tdprop tail word not zero");
	}
	return walked;
}

int ane_m2_program_build(const void *anec, uint64_t anec_size,
			 struct ane_m2_model *model,
			 struct ane_m2_sections *secs)
{
	const uint8_t *d = anec;
	uint64_t payload, tsk_size, krn_size, gen_size;
	uint32_t td_size, td_count, src_count, dst_count;
	uint32_t task_words, k;
	uint8_t *desc, *kern, *gen, *oper, *proc, *tdp;
	struct ane_m2_ref *refs;
	int blocks, err;

	memset(secs, 0, sizeof(*secs));
	memset(model, 0, sizeof(*model));

	if (anec_size < ANEC_M2_HEADER_SIZE + CONST_OFFSET) {
		return fail("anec shorter than the H14 header layout");
	}

	payload = le64(d);
	td_size = le32(d + 0x08);
	td_count = le32(d + 0x0c);
	tsk_size = le64(d + 0x10);
	krn_size = le64(d + 0x18);
	src_count = le32(d + 0x20);
	dst_count = le32(d + 0x24);

	if (payload != anec_size - ANEC_M2_HEADER_SIZE) {
		return fail("header payload size disagrees with file size");
	}
	/* One task per program: multi-task streams were never loaded. */
	if (td_count != 1) {
		return fail("td_count != 1");
	}
	if (td_size < FRAME_BYTES || td_size % 4) {
		return fail("bad firstTaskBytes");
	}
	if (tsk_size != FRAME_BYTES + td_size) {
		return fail("tsk_size != frame + task");
	}
	/* The constant region is 16384 B at 0x1140 in the proven layout. */
	if (krn_size != ANE_M2_KRN_BYTES) {
		return fail("krn_size != 0x4000");
	}
	if (anec_size != CONST_OFFSET + krn_size) {
		return fail("anec size != task region + constants");
	}
	/* One output, one or more inputs: the proven shape is 2 + 1. */
	if (dst_count != 1 || !src_count ||
	    src_count + dst_count > ANE_M2_MAX_BINDS) {
		return fail("unsupported src/dst counts");
	}

	/* io table: H14 channel contract (H14Program.cpp:547-561, proven on
	 * hardware): output = channel 4, input k = channel 5+k. Emission
	 * order: inputs ascending, then the output. */
	model->io_count = src_count + dst_count;
	for (k = 0; k < model->io_count; k++) {
		struct ane_m2_io *io = &model->io[k];
		uint32_t b;
		uint64_t n, c, plane, tiles;

		if (k < src_count) {
			b = 5 + k;
			io->dir = 0;
		} else {
			b = 4;
			io->dir = 1;
		}
		/* The call checker rejects io ids equal to the kernel/text
		 * section ids 2/3 (fw135 0x48ee0-0x48f10); ids >= 5 cannot
		 * hit them, but stay explicit. */
		if (b >= TILE_COUNT || b == 2 || b == 3) {
			return fail("io channel id collides with a section id");
		}
		/* Entry size from the anec geometry: N * C * plane bytes,
		 * cross-checked against tiles[b] << 14 (the add fixture
		 * carries nchw [1,512,1,1,64,64] and tiles[b] == 2, both
		 * 0x8000). Refuse on disagreement instead of guessing. */
		n = le64(d + 0xa8 + b * 48);
		c = le64(d + 0xa8 + b * 48 + 8);
		plane = le64(d + 0xa8 + b * 48 + 32);
		tiles = le32(d + 0x28 + b * 4);
		if (!n || !c || !plane || n > (1ULL << 20) || c > (1ULL << 20) ||
		    plane > (1ULL << 20) || n * c * plane > (1ULL << 30)) {
			return fail("unusable nchw geometry for an io channel");
		}
		io->buffer_id = b;
		io->size = n * c * plane;
		if (!tiles || (tiles << ANE_M2_TILE_UNIT_SHIFT) != io->size) {
			return fail("tiles[] disagrees with the nchw size");
		}
	}

	/* Descriptor section: 16-byte zero frame + the task. The task stream
	 * header word (task word 4) declares the task length in bits 26:16;
	 * copying only firstTaskBytes drops the tail records and wedges the
	 * TQ, so the section is frame + task and the two lengths must agree. */
	desc = sec_alloc(secs, ANE_M2_SEC_DESCRIPTOR, FRAME_BYTES + td_size);
	if (!desc) {
		return -ENOMEM;
	}
	memcpy(desc, d + TASK_OFFSET, FRAME_BYTES + td_size);
	for (k = 0; k < FRAME_BYTES; k++) {
		if (desc[k]) {
			return fail("task frame not zero");
		}
	}
	task_words = (le32(desc + FRAME_BYTES) >> 16) & 0x7ff;
	if (4 + task_words != (FRAME_BYTES + td_size) / 4) {
		return fail("task header word count disagrees with td_size");
	}

	refs = calloc(model->io_count, sizeof(struct ane_m2_ref));
	if (!refs) {
		return -ENOMEM;
	}
	err = walk_bar_records(desc, FRAME_BYTES + td_size, model, refs,
			       &model->ref_count);
	if (err) {
		free(refs);
		return err;
	}
	memcpy(model->refs, refs, model->ref_count * sizeof(*refs));
	free(refs);

	/* Kernel/constant section: the raw constant region. */
	kern = sec_alloc(secs, ANE_M2_SEC_KERNEL, krn_size);
	if (!kern) {
		return -ENOMEM;
	}
	memcpy(kern, d + CONST_OFFSET, krn_size);

	/* Generic section: 0x208-byte header + 0x30-byte entries. */
	gen_size = 0x208 + 0x30ULL * model->io_count;
	gen = sec_alloc(secs, ANE_M2_SEC_GENERIC, gen_size);
	if (!gen) {
		return -ENOMEM;
	}
	put_le32(gen + 0x00, 1);    /* magic (fw135 0x481fc) */
	put_le32(gen + 0x04, 0x10); /* version <= 0x10 (0x48208) */
	put_le32(gen + 0x204, model->io_count);
	for (k = 0; k < model->io_count; k++) {
		const struct ane_m2_io *io = &model->io[k];
		uint8_t *e = gen + 0x208 + 0x30ULL * k;

		put_le32(e + 0x00, 1); /* flags: present */
		put_le32(e + 0x04, io->buffer_id);
		put_le32(e + 0x08, io->dir); /* type: 0 in / 1 out */
		put_le32(e + 0x10, io->dir ? 2 : 1); /* io direction */
		put_le64(e + 0x20, io->size);
		put_le32(e + 0x28, 0xffff); /* sentinel, mirrored from h14conv */
	}

	/* Operation section: u32 tot + one 0x40c-byte record. type 0 (kernel
	 * op) with tdCount 0 short-circuits the kernel-ref resolver
	 * (fw135 0x48904); refCount refs {slot, tag} at +0x10. */
	oper = sec_alloc(secs, ANE_M2_SEC_OPERATION, 4 + 0x40c);
	if (!oper) {
		return -ENOMEM;
	}
	put_le32(oper, 1);
	put_le32(oper + 4 + 0x08, model->ref_count);
	for (k = 0; k < model->ref_count; k++) {
		put_le32(oper + 4 + 0x0c + 8ULL * k, model->refs[k].slot);
		put_le32(oper + 4 + 0x0c + 8ULL * k + 4, model->refs[k].tag);
	}

	/* Procedure section: tot + {u64 offset, u64 size} + records. Content
	 * mirrors the h14conv procedure.bin (load-proven on hardware);
	 * record +4 = 3 is eCSneCmdProgramProcedureContentType_3, accepted
	 * by getProcedureCallType with {0,3,4} (fw135 0x5a04c region). No
	 * rule for other contents was derived, so it stays fixed. */
	proc = sec_alloc(secs, ANE_M2_SEC_PROCEDURE, 0x18 + 0x20);
	if (!proc) {
		return -ENOMEM;
	}
	put_le32(proc, 1);
	put_le64(proc + 0x08, 0x18);
	put_le64(proc + 0x10, 0x20);
	put_le32(proc + 0x18, 1);
	put_le32(proc + 0x1c, 3);
	put_le32(proc + 0x20, 0);
	put_le32(proc + 0x24, 0);
	put_le32(proc + 0x28, 1);
	put_le32(proc + 0x2c, 0);
	put_le32(proc + 0x30, 0xffffffff);
	put_le32(proc + 0x34, 4);

	/* tdprop: one segment covering the whole descriptor, blockNbr from
	 * the firmware block walk. */
	blocks = tdprop_block_count(desc, FRAME_BYTES + td_size);
	if (blocks < 0) {
		return blocks;
	}
	tdp = sec_alloc(secs, ANE_M2_SEC_TDPROP, 40);
	if (!tdp) {
		return -ENOMEM;
	}
	put_le32(tdp, 1);
	put_le32(tdp + 0x08, 0); /* a */
	put_le32(tdp + 0x0c, (uint32_t)blocks);
	put_le64(tdp + 0x10, 0); /* segment offset */
	put_le64(tdp + 0x18, 0); /* pad */
	put_le64(tdp + 0x20, FRAME_BYTES + td_size);

	model->td_size = td_size;
	return 0;
}

void ane_m2_sections_free(struct ane_m2_sections *secs)
{
	int i;

	for (i = 0; i < ANE_M2_SEC_COUNT; i++) {
		free(secs->sec[i].data);
	}
	memset(secs, 0, sizeof(*secs));
}

/* ---- device path ---- */

struct ane_m2_ctx {
	struct ane_m2_model model;
	struct ane_m2_sections secs; /* CPU copies, kept for debugging */
	uint32_t prog_id;
	uint32_t proc_id;
	struct ane_bo sec_bo[ANE_M2_SEC_COUNT];
	struct ane_bo io_bo[ANE_M2_MAX_BINDS];
};

static int bo_alloc(struct ane_nn *nn, struct ane_bo *bo, uint64_t size)
{
	struct drm_ane_bo_init args = { .size = size };
	struct drm_ane_bo_free f;

	if (ioctl(nn->fd, DRM_IOCTL_ANE_BO_INIT, &args) < 0) {
		ane_m2_err("DRM_IOCTL_ANE_BO_INIT failed for %llu bytes\n",
			   (unsigned long long)size);
		return -EINVAL;
	}
	bo->size = size;
	bo->handle = args.handle;
	bo->offset = args.offset;
	bo->map = mmap(0, bo->size, PROT_READ | PROT_WRITE, MAP_SHARED, nn->fd,
		       bo->offset);
	if (bo->map == MAP_FAILED) {
		bo->map = NULL;
		f.handle = bo->handle;
		ioctl(nn->fd, DRM_IOCTL_ANE_BO_FREE, &f);
		bo->handle = 0;
		ane_m2_err("failed to mmap BO of %llu bytes\n",
			   (unsigned long long)size);
		return -EINVAL;
	}
	return 0;
}

/* BO_FREE releases only the CPU mapping and handle; the kernel keeps every
 * IOVA in this program reserved until reboot (IOVA lifetime v1), so freeing
 * is safe and the program stays kernel-visible. */
static void bo_release(struct ane_nn *nn, struct ane_bo *bo)
{
	struct drm_ane_bo_free f;

	if (bo->map) {
		munmap(bo->map, bo->size);
		bo->map = NULL;
	}
	if (bo->handle) {
		f.handle = bo->handle;
		ioctl(nn->fd, DRM_IOCTL_ANE_BO_FREE, &f);
		bo->handle = 0;
	}
}

static void ane_m2_ctx_free(struct ane_nn *nn, struct ane_m2_ctx *ctx,
			    int created_secs, int created_ios)
{
	int i;

	for (i = 0; i < created_secs; i++) {
		bo_release(nn, &ctx->sec_bo[i]);
	}
	for (i = 0; i < created_ios; i++) {
		bo_release(nn, &ctx->io_bo[i]);
	}
	ane_m2_sections_free(&ctx->secs);
	free(ctx);
}

static int ane_m2_fread_all(const char *path, void **out, uint64_t *out_size)
{
	FILE *fp;
	long end;
	uint8_t *buf;

	fp = fopen(path, "rb");
	if (!fp) {
		ane_m2_err("failed to open anec %s\n", path);
		return -EINVAL;
	}
	if (fseek(fp, 0, SEEK_END)) {
		fclose(fp);
		return -EINVAL;
	}
	end = ftell(fp);
	if (end < 0 || fseek(fp, 0, SEEK_SET)) {
		fclose(fp);
		return -EINVAL;
	}
	buf = malloc((uint64_t)end);
	if (!buf) {
		fclose(fp);
		return -ENOMEM;
	}
	if (fread(buf, 1, (uint64_t)end, fp) != (uint64_t)end) {
		ane_m2_err("short read on %s\n", path);
		free(buf);
		fclose(fp);
		return -EINVAL;
	}
	fclose(fp);
	*out = buf;
	*out_size = (uint64_t)end;
	return 0;
}

int ane_m2_open(struct ane_nn *nn, const char *path)
{
	struct ane_m2_ctx *ctx;
	struct drm_ane_section sec_args[ANE_M2_SEC_COUNT];
	struct drm_ane_generic_bind binds[ANE_M2_MAX_BINDS];
	struct drm_ane_prog_load load;
	struct drm_ane_proc_create create;
	uint8_t *buf = NULL;
	uint64_t size = 0;
	uint32_t i;
	int created_secs = 0;
	int created_ios = 0;
	int err;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}

	err = ane_m2_fread_all(path, (void **)&buf, &size);
	if (err) {
		free(ctx);
		return err;
	}
	if (size >= sizeof(struct anec)) {
		memcpy(&nn->anec, buf, sizeof(struct anec));
	}

	err = ane_m2_program_build(buf, size, &ctx->model, &ctx->secs);
	free(buf);
	if (err) {
		ane_m2_err("failed to build sections from %s\n", path);
		free(ctx);
		return err;
	}

	for (i = 0; i < ANE_M2_SEC_COUNT; i++, created_secs++) {
		err = bo_alloc(nn, &ctx->sec_bo[i], ctx->secs.sec[i].size);
		if (err) {
			goto error;
		}
		memcpy(ctx->sec_bo[i].map, ctx->secs.sec[i].data,
		       ctx->secs.sec[i].size);
	}

	for (i = 0; i < ctx->model.io_count; i++, created_ios++) {
		err = bo_alloc(nn, &ctx->io_bo[i], ctx->model.io[i].size);
		if (err) {
			goto error;
		}
		memset(ctx->io_bo[i].map, 0, ctx->model.io[i].size);
	}

	/* PROG_LOAD: section list plus one generic bind per io buffer; the
	 * driver patches each generic entry with the buffer's IOVA and size. */
	for (i = 0; i < ANE_M2_SEC_COUNT; i++) {
		sec_args[i].id = ane_m2_section_ids[i];
		sec_args[i].bo_handle = ctx->sec_bo[i].handle;
		sec_args[i].size = ctx->secs.sec[i].size;
		sec_args[i].offset = 0;
	}
	for (i = 0; i < ctx->model.io_count; i++) {
		binds[i].buffer_id = ctx->model.io[i].buffer_id;
		binds[i].bo_handle = ctx->io_bo[i].handle;
		binds[i].type = ctx->model.io[i].dir;
		binds[i].size = ctx->model.io[i].size;
	}
	load.sections_ptr = (uint64_t)(uintptr_t)sec_args;
	load.generic_ptr = (uint64_t)(uintptr_t)binds;
	load.section_count = ANE_M2_SEC_COUNT;
	load.generic_count = ctx->model.io_count;
	load.prog_id_out = 0;
	load.pad = 0;
	if (ioctl(nn->fd, DRM_IOCTL_ANE_PROG_LOAD, &load) < 0) {
		ane_m2_err("DRM_IOCTL_ANE_PROG_LOAD failed\n");
		goto error;
	}
	ctx->prog_id = load.prog_id_out;

	create.prog_id = ctx->prog_id;
	create.proc_id_out = 0;
	if (ioctl(nn->fd, DRM_IOCTL_ANE_PROC_CREATE, &create) < 0) {
		ane_m2_err("DRM_IOCTL_ANE_PROC_CREATE failed\n");
		goto error;
	}
	ctx->proc_id = create.proc_id_out;

	nn->m2 = ctx;
	return 0;

error:
	ane_m2_ctx_free(nn, ctx, created_secs, created_ios);
	return err;
}

void ane_m2_close(struct ane_nn *nn)
{
	struct ane_m2_ctx *ctx = nn->m2;

	if (!ctx) {
		return;
	}
	nn->m2 = NULL;
	ane_m2_ctx_free(nn, ctx, ANE_M2_SEC_COUNT, (int)ctx->model.io_count);
}

int ane_m2_exec(struct ane_nn *nn)
{
	struct ane_m2_ctx *ctx = nn->m2;
	struct drm_ane_exec_io io[ANE_M2_MAX_BINDS];
	struct drm_ane_exec args;
	uint32_t i;

	if (!ctx) {
		return -EINVAL;
	}
	for (i = 0; i < ctx->model.io_count; i++) {
		io[i].buffer_id = ctx->model.io[i].buffer_id;
		io[i].bo_handle = ctx->io_bo[i].handle;
		io[i].type = ctx->model.io[i].dir;
		io[i].flags = 0;
		io[i].dma = 0;
		io[i].size = ctx->model.io[i].size;
	}
	args.prog_id = ctx->prog_id;
	args.proc_id = ctx->proc_id;
	args.priority = ANE_M2_PRIORITY_DEFAULT;
	args.timeout_ms = ANE_M2_EXEC_TIMEOUT_MS;
	args.count = ctx->model.io_count;
	args.pad = 0;
	args.io_ptr = (uint64_t)(uintptr_t)io;
	if (ioctl(nn->fd, DRM_IOCTL_ANE_EXEC, &args) < 0) {
		ane_m2_err("DRM_IOCTL_ANE_EXEC failed\n");
		return -EINVAL;
	}
	return 0;
}

static struct ane_m2_io *nth_io(struct ane_m2_ctx *ctx, uint32_t dir,
				uint32_t idx)
{
	uint32_t seen = 0;
	uint32_t i;

	for (i = 0; i < ctx->model.io_count; i++) {
		if (ctx->model.io[i].dir != dir) {
			continue;
		}
		if (seen == idx) {
			return &ctx->model.io[i];
		}
		seen++;
	}
	return NULL;
}

int ane_m2_send(struct ane_nn *nn, const void *from, uint32_t idx)
{
	struct ane_m2_ctx *ctx = nn->m2;
	struct ane_m2_io *io;

	if (!ctx || !(io = nth_io(ctx, 0, idx))) {
		return -EINVAL;
	}
	memcpy(ctx->io_bo[io - ctx->model.io].map, from, io->size);
	return 0;
}

int ane_m2_read(struct ane_nn *nn, void *to, uint32_t idx)
{
	struct ane_m2_ctx *ctx = nn->m2;
	struct ane_m2_io *io;

	if (!ctx || !(io = nth_io(ctx, 1, idx))) {
		return -EINVAL;
	}
	memcpy(to, ctx->io_bo[io - ctx->model.io].map, io->size);
	return 0;
}

uint64_t ane_m2_src_size(struct ane_nn *nn, uint32_t idx)
{
	struct ane_m2_io *io;

	if (!nn->m2 || !(io = nth_io(nn->m2, 0, idx))) {
		return 0;
	}
	return io->size;
}

uint64_t ane_m2_dst_size(struct ane_nn *nn, uint32_t idx)
{
	struct ane_m2_io *io;

	if (!nn->m2 || !(io = nth_io(nn->m2, 1, idx))) {
		return 0;
	}
	return io->size;
}
