// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

/*
 * Host-only tests for the ABI-3 (in-tree aurora H15/H16/H17) backend in
 * libane/ane_v3.c and libane/ane.c.
 *
 * Covers:
 *  1. H15 (T8122, subtype 6, 8-word task header) 4-section build against the
 *     real fixtures/h15-hwx/add/model.hwx and its reference procedure/property
 *     sections.
 *  2. End-to-end ane_init / ane_send / ane_exec / ane_read / ane_free against
 *     a wrapped /dev/accel/accel0 node (abi 3, chip_family 15):
 *     - minor 1 in-place mmap + DRM_ANE_H16_EXEC_IN_PLACE (zero-copy)
 *     - minor 1 mmap -EOPNOTSUPP automatic fallback to copy-mode EXEC
 *     - minor 0 copy-mode EXEC
 *  3. Synthetic H16 (subtype 7, 9-word header) with __KERN_0 (resource 1007)
 *     and __DATA scratch entry.
 *  4. Negative cases: bad magic, unsupported subtype, truncated stream,
 *     H16_LOAD failure cleanup, batch > 1 (-ENOSYS), exec_loop (-EOPNOTSUPP).
 *
 * usage: test_libane_v3 <model.hwx> <load-base-dir>
 */

#define _GNU_SOURCE
#include <drm.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <ane_accel.h>
#include "ane.h"
#include "ane_v3.h"

#define FAKE_NODE "/dev/accel/accel0"

static int fake_fd = -1;
static int fake_minor = 1;
static int fake_mmap_fail = 0;
static int fake_load_fail_errno = 0;
static int fake_load_calls = 0;
static int fake_exec_calls = 0;
static int fake_exec_in_place_calls = 0;
static int fake_exec_copy_calls = 0;
static int fake_mmap_calls = 0;
static int fake_munmap_calls = 0;
static uint8_t *fake_surfaces[DRM_ANE_H16_SURFACES];
static uint64_t fake_surface_size[DRM_ANE_H16_SURFACES];
static uint32_t fake_surface_dir[DRM_ANE_H16_SURFACES];
static uint32_t fake_surface_count = 0;
static int failures = 0;

int __real_open(const char *path, int flags, ...);
int __real_ioctl(int fd, unsigned long request, ...);
void *__real_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int __real_munmap(void *addr, size_t length);

static void check_cond(int cond, const char *what)
{
	if (!cond) {
		printf("FAIL %s\n", what);
		failures++;
		return;
	}
	printf("ok   %s\n", what);
}

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

static void reset_fake_surfaces(void)
{
	uint32_t i;

	for (i = 0; i < DRM_ANE_H16_SURFACES; i++) {
		free(fake_surfaces[i]);
		fake_surfaces[i] = NULL;
		fake_surface_size[i] = 0;
		fake_surface_dir[i] = 0;
	}
	fake_surface_count = 0;
}

/* Fake engine execution: out[k] = in0[k] ^ in1[k] (distinct non-trivial transform). */
static void fake_run_compute(void)
{
	uint32_t i;
	const uint8_t *in0 = NULL;
	const uint8_t *in1 = NULL;
	uint8_t *out = NULL;
	uint64_t sz = 0;

	for (i = 0; i < fake_surface_count; i++) {
		if (fake_surface_dir[i] == 1u) {
			if (!in0)
				in0 = fake_surfaces[i];
			else
				in1 = fake_surfaces[i];
		} else if (fake_surface_dir[i] == 2u) {
			out = fake_surfaces[i];
			sz = fake_surface_size[i];
		}
	}
	if (in0 && in1 && out) {
		uint64_t k;

		for (k = 0; k < sz; k++)
			out[k] = (uint8_t)(in0[k] ^ in1[k]);
	}
}

int __wrap_open(const char *path, int flags, ...)
{
	if (flags & O_CREAT) {
		va_list ap;

		va_start(ap, flags);
		(void)va_arg(ap, mode_t);
		va_end(ap);
	}
	if (!strcmp(path, FAKE_NODE)) {
		fake_fd = __real_open("/dev/null", O_RDWR | O_CLOEXEC);
		return fake_fd;
	}
	if (!strncmp(path, "/dev/accel/accel", 16)) {
		errno = ENOENT;
		return -1;
	}
	return __real_open(path, flags);
}

void *__wrap_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	uint64_t uoff = (uint64_t)offset;
	uint32_t idx;

	if (fd != fake_fd)
		return __real_mmap(addr, length, prot, flags, fd, offset);
	fake_mmap_calls++;
	if (fake_mmap_fail) {
		errno = EOPNOTSUPP;
		return MAP_FAILED;
	}
	if ((uoff & 0xffffffffULL) != 0 || (uoff >> 32) < 1 ||
	    (uoff >> 32) > fake_surface_count) {
		errno = EINVAL;
		return MAP_FAILED;
	}
	idx = (uint32_t)((uoff >> 32) - 1);
	if (length != fake_surface_size[idx]) {
		errno = EINVAL;
		return MAP_FAILED;
	}
	if ((prot & PROT_WRITE) && fake_surface_dir[idx] != 1u) {
		errno = EINVAL;
		return MAP_FAILED;
	}
	return fake_surfaces[idx];
}

int __wrap_munmap(void *addr, size_t length)
{
	uint32_t i;

	for (i = 0; i < fake_surface_count; i++) {
		if (fake_surfaces[i] && addr == fake_surfaces[i]) {
			fake_munmap_calls++;
			return 0;
		}
	}
	return __real_munmap(addr, length);
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	void *arg = NULL;
	va_list ap;

	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);

	if (fd != fake_fd) {
		errno = EBADF;
		return -1;
	}

	if (request == DRM_IOCTL_VERSION) {
		drm_version_t *v = (drm_version_t *)arg;

		if (v->name && v->name_len >= 3)
			memcpy(v->name, "ane", 3);
		v->name_len = 3;
		v->date_len = 0;
		v->desc_len = 0;
		v->version_major = ANE_ABI_V3_MAJOR;
		v->version_minor = fake_minor;
		v->version_patchlevel = 0;
		return 0;
	}

	if (request == ANE_V3_GET_CAPS_IOCTL) {
		struct ane_v3_get_caps *caps = (struct ane_v3_get_caps *)arg;

		if (caps->flags || caps->pad) {
			errno = EINVAL;
			return -1;
		}
		caps->size = sizeof(*caps);
		caps->abi_version = ANE_ABI_V3_MAJOR;
		caps->chip_family = ANE_CHIP_H15;
		caps->section_size = sizeof(struct drm_ane_h16_section);
		caps->bind_size = 0;
		caps->exec_io_size = sizeof(struct drm_ane_h16_buffer);
		return 0;
	}

	if (request == DRM_IOCTL_ANE_H16_LOAD) {
		struct drm_ane_h16_load *load = (struct drm_ane_h16_load *)arg;
		const struct drm_ane_h16_section *secs;
		const uint8_t *gen;
		uint32_t i;

		fake_load_calls++;
		if (fake_load_fail_errno) {
			errno = fake_load_fail_errno;
			return -1;
		}
		if (load->flags || load->pad ||
		    load->section_count != DRM_ANE_H16_SECTIONS ||
		    !load->hwx_ptr || load->hwx_size < 32 || !load->sections_ptr) {
			errno = EINVAL;
			return -1;
		}
		secs = (const struct drm_ane_h16_section *)(uintptr_t)load->sections_ptr;
		for (i = 0; i < DRM_ANE_H16_SECTIONS; i++) {
			if (secs[i].kind != DRM_ANE_H16_SECTION_GENERIC + i ||
			    secs[i].size < DRM_ANE_H16_SECTION_MIN ||
			    secs[i].size > DRM_ANE_H16_SECTION_MAX ||
			    (secs[i].size & 3u) != 0 || !secs[i].data_ptr) {
				errno = EINVAL;
				return -1;
			}
		}
		gen = (const uint8_t *)(uintptr_t)secs[0].data_ptr;
		reset_fake_surfaces();
		fake_surface_count = rd_u32(gen, 0x204);
		if (fake_surface_count < 2 || fake_surface_count > DRM_ANE_H16_SURFACES) {
			errno = ENOEXEC;
			return -1;
		}
		load->surface_count = fake_surface_count;
		for (i = 0; i < fake_surface_count; i++) {
			size_t e = 0x208 + (size_t)i * 0x30;

			fake_surface_dir[i] = rd_u32(gen, e + 0x10);
			fake_surface_size[i] = rd_u64(gen, e + 0x20);
			fake_surfaces[i] = calloc(1, (size_t)fake_surface_size[i]);
			load->surface_size[i] = fake_surface_size[i];
			load->surface_direction[i] = fake_surface_dir[i];
		}
		return 0;
	}

	if (request == DRM_IOCTL_ANE_H16_EXEC) {
		const struct drm_ane_h16_exec *exec = (const struct drm_ane_h16_exec *)arg;
		uint32_t i;

		fake_exec_calls++;
		if ((exec->flags & ~DRM_ANE_H16_EXEC_IN_PLACE) != 0 ||
		    exec->count != fake_surface_count) {
			errno = EINVAL;
			return -1;
		}
		if (exec->flags & DRM_ANE_H16_EXEC_IN_PLACE) {
			if (exec->buffers_ptr != 0) {
				errno = EINVAL;
				return -1;
			}
			fake_exec_in_place_calls++;
			fake_run_compute();
			return 0;
		}
		if (!exec->buffers_ptr) {
			errno = EINVAL;
			return -1;
		}
		fake_exec_copy_calls++;
		{
			const struct drm_ane_h16_buffer *bufs =
				(const struct drm_ane_h16_buffer *)(uintptr_t)exec->buffers_ptr;

			for (i = 0; i < exec->count; i++) {
				if (bufs[i].size != fake_surface_size[i] || !bufs[i].ptr) {
					errno = EINVAL;
					return -1;
				}
				if (fake_surface_dir[i] == 1u) {
					memcpy(fake_surfaces[i],
					       (const void *)(uintptr_t)bufs[i].ptr,
					       (size_t)bufs[i].size);
				}
			}
			fake_run_compute();
			for (i = 0; i < exec->count; i++) {
				if (fake_surface_dir[i] == 2u) {
					memcpy((void *)(uintptr_t)bufs[i].ptr,
					       fake_surfaces[i],
					       (size_t)bufs[i].size);
				}
			}
		}
		return 0;
	}

	errno = ENOTTY;
	return -1;
}

static uint8_t *load_file(const char *path, size_t *size_out)
{
	FILE *fp = fopen(path, "rb");
	long len;
	uint8_t *buf;

	if (!fp)
		return NULL;
	if (fseek(fp, 0, SEEK_END) != 0) {
		fclose(fp);
		return NULL;
	}
	len = ftell(fp);
	if (len <= 0 || fseek(fp, 0, SEEK_SET) != 0) {
		fclose(fp);
		return NULL;
	}
	buf = malloc((size_t)len);
	if (!buf) {
		fclose(fp);
		return NULL;
	}
	if (fread(buf, 1, (size_t)len, fp) != (size_t)len) {
		free(buf);
		fclose(fp);
		return NULL;
	}
	fclose(fp);
	*size_out = (size_t)len;
	return buf;
}

static void test_h15_sections(const char *hwx_path, const char *base_dir)
{
	size_t hwx_sz = 0;
	size_t proc_sz = 0;
	size_t prop_sz = 0;
	uint8_t *hwx = load_file(hwx_path, &hwx_sz);
	char ppath[512];
	uint8_t *ref_proc;
	uint8_t *ref_prop;
	struct ane_v3_sections secs;
	const uint8_t *gen;
	const uint8_t *op;
	int err;

	check_cond(hwx != NULL, "read H15 add model.hwx");
	if (!hwx)
		return;

	err = ane_v3_build_sections(hwx, hwx_sz, &secs);
	check_cond(err == 0, "ane_v3_build_sections on H15 add model.hwx");
	if (err == 0) {
		check_cond(secs.subtype == ANE_V3_HWX_SUBTYPE_H15, "H15 subtype == 6");
		check_cond(secs.tasks == 1u, "H15 task count == 1");
		check_cond(secs.surface_count == 3u, "H15 surface count == 3");
		check_cond(secs.surface_direction[0] == 1u &&
			   secs.surface_direction[1] == 1u &&
			   secs.surface_direction[2] == 2u,
			   "H15 surface directions [1, 1, 2]");
		check_cond(secs.surface_id[0] == 5u &&
			   secs.surface_id[1] == 6u &&
			   secs.surface_id[2] == 4u,
			   "H15 ABI-3 generic buffer IDs [5, 6, 4]");
		check_cond(secs.size[0] == 0x298u && secs.size[1] == 0x410u &&
			   secs.size[2] == 0x38u && secs.size[3] == 0x28u,
			   "H15 4 section sizes [0x298, 0x410, 0x38, 0x28]");

		gen = (const uint8_t *)secs.data[0];
		check_cond(rd_u32(gen, 0) == 1u && rd_u32(gen, 4) == 0x10u &&
			   rd_u32(gen, 0x204) == 3u,
			   "H15 generic header magic/version/count");
		check_cond(rd_u32(gen, 0x208 + 0x04) == 5u &&
			   rd_u32(gen, 0x208 + 0x30 + 0x04) == 6u &&
			   rd_u32(gen, 0x208 + 0x60 + 0x04) == 4u &&
			   rd_u64(gen, 0x208 + 0x20) == 32768u &&
			   rd_u32(gen, 0x208 + 0x28) == 0xffffu,
			   "H15 generic entries match ABI-3 spec");

		op = (const uint8_t *)secs.data[1];
		check_cond(rd_u32(op, 0) == 1u && rd_u32(op, 4 + 8) == 3u &&
			   rd_u32(op, 4 + 0x0c) == 6u && rd_u32(op, 4 + 0x10) == 5u &&
			   rd_u32(op, 4 + 0x14) == 7u && rd_u32(op, 4 + 0x18) == 6u &&
			   rd_u32(op, 4 + 0x1c) == 8u && rd_u32(op, 4 + 0x20) == 4u,
			   "H15 operation BAR pairs [(6,5), (7,6), (8,4)]");

		snprintf(ppath, sizeof(ppath), "%s/procedure.bin", base_dir);
		ref_proc = load_file(ppath, &proc_sz);
		check_cond(ref_proc != NULL && proc_sz == secs.size[2] &&
			   memcmp(ref_proc, secs.data[2], proc_sz) == 0,
			   "H15 procedure.bin byte-identical to fixture reference");
		free(ref_proc);

		snprintf(ppath, sizeof(ppath), "%s/property.bin", base_dir);
		ref_prop = load_file(ppath, &prop_sz);
		check_cond(ref_prop != NULL && prop_sz == secs.size[3] &&
			   memcmp(ref_prop, secs.data[3], prop_sz) == 0,
			   "H15 text-properties.bin byte-identical to fixture reference");
		free(ref_prop);

		ane_v3_sections_free(&secs);
	}

	/* Negative: corrupt magic, bad subtype, truncated buffer */
	hwx[0] ^= 0xff;
	check_cond(ane_v3_build_sections(hwx, hwx_sz, &secs) == -ENOEXEC,
		   "reject corrupt HWX magic");
	hwx[0] ^= 0xff;

	wr_u32(hwx, 8, 5u); /* H14 subtype not ABI 3 */
	check_cond(ane_v3_build_sections(hwx, hwx_sz, &secs) == -ENOEXEC,
		   "reject unsupported HWX subtype 5");
	wr_u32(hwx, 8, ANE_V3_HWX_SUBTYPE_H15);

	check_cond(ane_v3_build_sections(hwx, 16, &secs) == -EINVAL,
		   "reject truncated HWX");

	free(hwx);
}

static void test_runtime_mode(const char *hwx_path, int minor, int mmap_fail,
			      const char *label)
{
	struct ane_nn *nn;
	uint8_t in0[32768];
	uint8_t in1[32768];
	uint8_t out[32768];
	size_t i;
	int exact = 1;
	int prev_in_place = fake_exec_in_place_calls;
	int prev_copy = fake_exec_copy_calls;
	int prev_munmap = fake_munmap_calls;

	fake_minor = minor;
	fake_mmap_fail = mmap_fail;
	fake_load_fail_errno = 0;

	nn = ane_init(hwx_path);
	check_cond(nn != NULL, label);
	if (!nn)
		return;

	check_cond(ane_src_count(nn) == 2u && ane_dst_count(nn) == 1u,
		   "V3 src/dst counts (2 inputs, 1 output)");
	check_cond(ane_src_size(nn, 0) == 32768u &&
		   ane_src_size(nn, 1) == 32768u &&
		   ane_dst_size(nn, 0) == 32768u,
		   "V3 surface sizes 32768 B");

	for (i = 0; i < sizeof(in0); i++) {
		in0[i] = (uint8_t)((i * 37u + 11u) & 0xffu);
		in1[i] = (uint8_t)((i * 53u + 7u) & 0xffu);
		out[i] = 0xa5u;
	}

	ane_send(nn, in0, 0);
	ane_send(nn, in1, 1);
	check_cond(ane_exec(nn) == 0, "V3 ane_exec returns 0");
	ane_read(nn, out, 0);

	for (i = 0; i < sizeof(out); i++) {
		if (out[i] != (uint8_t)(in0[i] ^ in1[i])) {
			exact = 0;
			break;
		}
	}
	check_cond(exact, "V3 output bytes exact across 32768 B");

	if (minor >= 1 && !mmap_fail) {
		check_cond(fake_exec_in_place_calls == prev_in_place + 1 &&
			   fake_exec_copy_calls == prev_copy,
			   "V3 minor 1 uses DRM_ANE_H16_EXEC_IN_PLACE");
	} else {
		check_cond(fake_exec_copy_calls == prev_copy + 1 &&
			   fake_exec_in_place_calls == prev_in_place,
			   "V3 fallback uses copy-mode H16_EXEC");
	}

	check_cond(ane_exec_batch(nn, 1) == 0, "V3 ane_exec_batch(1) succeeds");
	check_cond(ane_exec_batch(nn, 2) == -ENOSYS, "V3 ane_exec_batch(2) -> -ENOSYS");
	check_cond(ane_exec_loop(nn, 1, 0, 0) == -EOPNOTSUPP,
		   "V3 ane_exec_loop -> -EOPNOTSUPP");

	ane_free(nn);
	if (minor >= 1 && !mmap_fail) {
		check_cond(fake_munmap_calls == prev_munmap + 3,
			   "V3 close unmaps all 3 mapped surfaces");
	}
}

/* Synthetic H16 (subtype 7, 9-word task header) with __KERN_0 + __DATA scratch. */
static void test_synthetic_h16_kernel_scratch(void)
{
	uint8_t *hwx = calloc(1, 0x10000);
	size_t pos = 32;
	size_t desc_off;
	size_t text_off = 0x4000;
	struct ane_v3_sections secs;
	const uint8_t *gen;
	const uint8_t *op;
	int err;

	if (!hwx)
		return;

	/* Header */
	wr_u32(hwx, 0, 0xbeeffaceu);
	wr_u32(hwx, 4, 0x80u);
	wr_u32(hwx, 8, ANE_V3_HWX_SUBTYPE_H16);
	wr_u32(hwx, 12, 2u);
	wr_u32(hwx, 16, 6u); /* 5 segments + 1 program descriptor */
	wr_u32(hwx, 24, 0x200000u);

	/* 1. __DATA scratch (vm 0x30000000, size 0x4000, prot 3) */
	wr_u32(hwx, pos, 0x19u);
	wr_u32(hwx, pos + 4, 72u + 80u);
	memcpy(hwx + pos + 8, "__DATA", 6);
	wr_u64(hwx, pos + 24, 0x30000000ULL);
	wr_u64(hwx, pos + 32, 0x4000ULL);
	wr_u32(hwx, pos + 56, 3u);
	wr_u32(hwx, pos + 60, 3u);
	wr_u32(hwx, pos + 64, 1u);
	memcpy(hwx + pos + 72, "__bss", 5);
	memcpy(hwx + pos + 72 + 16, "__DATA", 6);
	wr_u64(hwx, pos + 72 + 32, 0x30000000ULL);
	wr_u64(hwx, pos + 72 + 40, 0x4000ULL);
	pos += 72u + 80u;

	/* 2. __FVMLIB input (vm 0x30004000, size 0x4000, prot 1) */
	wr_u32(hwx, pos, 0x19u);
	wr_u32(hwx, pos + 4, 72u + 80u);
	memcpy(hwx + pos + 8, "__FVMLIB", 8);
	wr_u64(hwx, pos + 24, 0x30004000ULL);
	wr_u64(hwx, pos + 32, 0x4000ULL);
	wr_u32(hwx, pos + 56, 1u);
	wr_u32(hwx, pos + 60, 1u);
	wr_u32(hwx, pos + 64, 1u);
	memcpy(hwx + pos + 72, "__const", 7);
	memcpy(hwx + pos + 72 + 16, "__FVMLIB", 8);
	wr_u64(hwx, pos + 72 + 32, 0x30004000ULL);
	wr_u64(hwx, pos + 72 + 40, 0x4000ULL);
	pos += 72u + 80u;

	/* 3. __FVMLIB output (vm 0x30008000, size 0x4000, prot 2) */
	wr_u32(hwx, pos, 0x19u);
	wr_u32(hwx, pos + 4, 72u + 80u);
	memcpy(hwx + pos + 8, "__FVMLIB", 8);
	wr_u64(hwx, pos + 24, 0x30008000ULL);
	wr_u64(hwx, pos + 32, 0x4000ULL);
	wr_u32(hwx, pos + 56, 2u);
	wr_u32(hwx, pos + 60, 2u);
	wr_u32(hwx, pos + 64, 1u);
	memcpy(hwx + pos + 72, "__data", 6);
	memcpy(hwx + pos + 72 + 16, "__FVMLIB", 8);
	wr_u64(hwx, pos + 72 + 32, 0x30008000ULL);
	wr_u64(hwx, pos + 72 + 40, 0x4000ULL);
	pos += 72u + 80u;

	/* 4. __TEXT with __text (size 0x4c, off 0x4000) */
	wr_u32(hwx, pos, 0x19u);
	wr_u32(hwx, pos + 4, 72u + 80u);
	memcpy(hwx + pos + 8, "__TEXT", 6);
	wr_u64(hwx, pos + 24, 0x3000c000ULL);
	wr_u64(hwx, pos + 32, 0x4000ULL);
	wr_u32(hwx, pos + 56, 5u);
	wr_u32(hwx, pos + 60, 5u);
	wr_u32(hwx, pos + 64, 1u);
	memcpy(hwx + pos + 72, "__text", 6);
	memcpy(hwx + pos + 72 + 16, "__TEXT", 6);
	wr_u64(hwx, pos + 72 + 32, 0x3000c000ULL);
	wr_u64(hwx, pos + 72 + 40, 0x4cu);
	wr_u32(hwx, pos + 72 + 48, (uint32_t)text_off);
	pos += 72u + 80u;

	/* 5. __KERN_0 with __kern_0 (vm 0x30010000, size 0x40) */
	wr_u32(hwx, pos, 0x19u);
	wr_u32(hwx, pos + 4, 72u + 80u);
	memcpy(hwx + pos + 8, "__KERN_0", 8);
	wr_u64(hwx, pos + 24, 0x30010000ULL);
	wr_u64(hwx, pos + 32, 0x4000ULL);
	wr_u32(hwx, pos + 56, 1u);
	wr_u32(hwx, pos + 60, 1u);
	wr_u32(hwx, pos + 64, 1u);
	memcpy(hwx + pos + 72, "__kern_0", 8);
	memcpy(hwx + pos + 72 + 16, "__KERN_0", 8);
	wr_u64(hwx, pos + 72 + 32, 0x30010000ULL);
	wr_u64(hwx, pos + 72 + 40, 0x40ULL);
	pos += 72u + 80u;

	/* 6. LC_UNIXTHREAD flavor 4 (0x8c0): slot 3=scratch, 6=in, 7=out, 8=kernel */
	desc_off = pos;
	wr_u32(hwx, pos, 4u);
	wr_u32(hwx, pos + 4, 0x8c0u);
	wr_u32(hwx, pos + 8, 4u);
	wr_u64(hwx, pos + 0x40, 0x30000000ULL); /* slot 3 = scratch */
	wr_u64(hwx, pos + 0x70, 0x30004000ULL); /* slot 6 = input */
	wr_u64(hwx, pos + 0x80, 0x30008000ULL); /* slot 7 = output */
	wr_u64(hwx, pos + 0x90, 0x30010000ULL); /* slot 8 = kernel */
	pos += 0x8c0u;
	(void)desc_off;

	wr_u32(hwx, 20, (uint32_t)(pos - 32));

	/* H16 task stream: 16B prologue(1) + 1 task (9-word hdr + 4 records of 1+1 words?
	 * Keep 1 task of 15 words = 60 B: 9 hdr + 4x 1-val reloc records:
	 *Wait: 9 + 4*2 = 17 words = 68 B -> 16 + 68 = 84 = 0x54 B! Update text size to 0x54. */
	wr_u64(hwx, 32 + 3 * (72u + 80u) + 72u + 40u, 0x54ULL);
	wr_u32(hwx, text_off + 0, 1u); /* H16 prologue == 1 */
	wr_u32(hwx, text_off + 16, 17u << 16); /* 17 words = 9 hdr + 8 rec words */
	/* Record 1: slot 3 (scratch) -> 0x4d10 */
	wr_u32(hwx, text_off + 16 + 36, (1u << 29) | (3u << 23) | (0x4d10u / 4u));
	/* Record 2: slot 6 (input) -> 0x4d28 */
	wr_u32(hwx, text_off + 16 + 44, (1u << 29) | (6u << 23) | (0x4d28u / 4u));
	/* Record 3: slot 7 (output) -> 0x5108 */
	wr_u32(hwx, text_off + 16 + 52, (1u << 29) | (7u << 23) | (0x5108u / 4u));
	/* Record 4: slot 8 (kernel) -> 0x5510 */
	wr_u32(hwx, text_off + 16 + 60, (1u << 29) | (8u << 23) | (0x5510u / 4u));

	err = ane_v3_build_sections(hwx, 0x10000, &secs);
	check_cond(err == 0, "synthetic H16 kernel+scratch build succeeds");
	if (err == 0) {
		gen = (const uint8_t *)secs.data[0];
		op = (const uint8_t *)secs.data[1];
		check_cond(rd_u32(gen, 0x204) == 3u && secs.has_scratch == 1 &&
			   secs.scratch_id == 6u,
			   "H16 generic appends scratch entry with next input ID 6");
		check_cond(rd_u32(op, 4 + 8) == 4u &&
			   rd_u32(op, 4 + 0x0c) == 3u && rd_u32(op, 4 + 0x10) == 6u &&
			   rd_u32(op, 4 + 0x14) == 6u && rd_u32(op, 4 + 0x18) == 5u &&
			   rd_u32(op, 4 + 0x1c) == 7u && rd_u32(op, 4 + 0x20) == 4u &&
			   rd_u32(op, 4 + 0x24) == 8u &&
			   rd_u32(op, 4 + 0x28) == DRM_ANE_H16_RESOURCE_KERNEL,
			   "H16 operation binds scratch(6), in(5), out(4), kernel(1007)");
		ane_v3_sections_free(&secs);
	}

	/* H16 refuses zero prologue */
	wr_u32(hwx, text_off + 0, 0u);
	check_cond(ane_v3_build_sections(hwx, 0x10000, &secs) == -ENOEXEC,
		   "H16 subtype 7 refuses zero prologue");

	free(hwx);
}

int main(int argc, char **argv)
{
	const char *hwx_path = argc > 1 ? argv[1] : "../fixtures/h15-hwx/add/model.hwx";
	const char *base_dir = argc > 2 ? argv[2] : "../fixtures/h15-hwx/add/load/base";
	struct ane_nn *nn;

	test_h15_sections(hwx_path, base_dir);
	test_synthetic_h16_kernel_scratch();

	test_runtime_mode(hwx_path, 1, 0, "V3 minor 1 in-place mmap open");
	test_runtime_mode(hwx_path, 1, 1, "V3 minor 1 mmap -EOPNOTSUPP fallback open");
	test_runtime_mode(hwx_path, 0, 0, "V3 minor 0 copy-mode open");

	/* Negative: kernel rejects H16_LOAD */
	fake_minor = 1;
	fake_mmap_fail = 0;
	fake_load_fail_errno = ENOEXEC;
	nn = ane_init(hwx_path);
	check_cond(nn == NULL, "V3 ane_init returns NULL on H16_LOAD -ENOEXEC");
	fake_load_fail_errno = 0;

	reset_fake_surfaces();
	if (failures) {
		printf("%d failure(s)\n", failures);
		return 1;
	}
	printf("all ABI-3 H15/H16/H17 userspace tests pass\n");
	return 0;
}
