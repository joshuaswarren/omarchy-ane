// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

#include <drm.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <ane_accel.h>
#include "ane.h"

/*
// HOST-ONLY ioctl argument check: no device, no kernel, no hardware.
//
// libane is compiled into this program with -ftrivial-auto-var-init=pattern
// (tools/Makefile), so a stack member that libane does not set holds 0xfe
// bytes, every run. The link wraps open, ioctl and mmap: open of
// /dev/accel/accel0 gives a fake DRM node (any other accel node is refused,
// so no real device is opened), and the fake answers DRM_IOCTL_VERSION and
// every ANE ioctl. Like the in-tree drivers, it refuses a nonzero pad,
// flags or reserved member with -EINVAL, and it checks that every BO handle
// is freed exactly once. The program file is a real H14 fixture. Nothing
// executes it: the fake checks only the argument structs.
//
// usage: test_libane_ioctl [fixtures-dir]   (default ../fixtures/h14-anec)
*/

#define FAKE_NODE "/dev/accel/accel0"
#define MAX_BOS 256

int __real_open(const char *path, int flags, ...);
int __real_ioctl(int fd, unsigned long request, ...);
void *__real_mmap(void *addr, size_t len, int prot, int flags, int fd,
		  off_t off);

static int abi_major;
static int fake_fd = -1;
static int mmap_countdown; /* nonzero: the Nth mmap of the fake node fails */
static int failures;
static uint32_t next_handle;
static uint8_t live[MAX_BOS + 1];

static int refuse(const char *what, uint64_t value)
{
	printf("FAIL %s = %#llx (the in-tree driver returns -EINVAL)\n", what,
	       (unsigned long long)value);
	failures++;
	errno = EINVAL;
	return -1;
}

int __wrap_open(const char *path, int flags, ...)
{
	mode_t mode = 0;
	va_list ap;

	if (flags & O_CREAT) {
		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	if (!strcmp(path, FAKE_NODE)) {
		fake_fd = __real_open("/dev/null", O_RDWR | O_CLOEXEC);
		return fake_fd;
	}
	if (!strncmp(path, "/dev/accel/", strlen("/dev/accel/"))) {
		errno = ENOENT;
		return -1;
	}
	return __real_open(path, flags, mode);
}

void *__wrap_mmap(void *addr, size_t len, int prot, int flags, int fd,
		  off_t off)
{
	if (fd != fake_fd) {
		return __real_mmap(addr, len, prot, flags, fd, off);
	}
	if (mmap_countdown && --mmap_countdown == 0) {
		errno = ENOMEM;
		return MAP_FAILED;
	}
	return __real_mmap(NULL, len, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

static int fake_version(drm_version_t *v)
{
	if (v->name && v->name_len >= 3) {
		memcpy(v->name, "ane", 3);
	}
	v->name_len = 3;
	v->date_len = 0;
	v->desc_len = 0;
	v->version_major = abi_major;
	v->version_minor = 0;
	v->version_patchlevel = 0;
	return 0;
}

static int fake_bo_init(struct drm_ane_bo_init *a)
{
	if (a->pad) {
		return refuse("drm_ane_bo_init.pad", a->pad);
	}
	if (!a->size || next_handle == MAX_BOS) {
		return refuse("drm_ane_bo_init.size", a->size);
	}
	a->handle = ++next_handle;
	a->offset = (uint64_t)a->handle << 32;
	live[a->handle] = 1;
	return 0;
}

static int fake_bo_free(const struct drm_ane_bo_free *a)
{
	if (a->pad) {
		return refuse("drm_ane_bo_free.pad", a->pad);
	}
	if (!a->handle || a->handle > next_handle || !live[a->handle]) {
		return refuse("drm_ane_bo_free.handle (not live)", a->handle);
	}
	live[a->handle] = 0;
	return 0;
}

static int fake_submit(const struct drm_ane_submit *a)
{
	if (a->pad) {
		return refuse("drm_ane_submit.pad", a->pad);
	}
	return 0;
}

static int fake_prog_load(struct drm_ane_prog_load *a)
{
	const struct drm_ane_generic_bind *b =
		(const void *)(uintptr_t)a->generic_ptr;
	uint32_t i;

	if (a->pad) {
		return refuse("drm_ane_prog_load.pad", a->pad);
	}
	if (a->generic_count > ANE_M2_MAX_BINDS) {
		return refuse("drm_ane_prog_load.generic_count",
			      a->generic_count);
	}
	for (i = 0; i < a->generic_count; i++) {
		if (b[i].pad) {
			return refuse("drm_ane_generic_bind.pad", b[i].pad);
		}
	}
	a->prog_id_out = 1;
	return 0;
}

static int fake_exec(const struct drm_ane_exec *a)
{
	const struct drm_ane_exec_io *io = (const void *)(uintptr_t)a->io_ptr;
	uint32_t i;

	if (a->pad) {
		return refuse("drm_ane_exec.pad", a->pad);
	}
	if (!a->count || a->count > ANE_M2_MAX_BINDS) {
		return refuse("drm_ane_exec.count", a->count);
	}
	for (i = 0; i < a->count; i++) {
		if (io[i].flags) {
			return refuse("drm_ane_exec_io.flags", io[i].flags);
		}
		/* dma is the in-tree header's reserved member. */
		if (io[i].dma) {
			return refuse("drm_ane_exec_io.dma", io[i].dma);
		}
	}
	return 0;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	void *arg;

	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);
	if (fd != fake_fd) {
		return __real_ioctl(fd, request, arg);
	}
	switch (request) {
	case DRM_IOCTL_VERSION:
		return fake_version(arg);
	case DRM_IOCTL_ANE_BO_INIT:
		return fake_bo_init(arg);
	case DRM_IOCTL_ANE_BO_FREE:
		return fake_bo_free(arg);
	case DRM_IOCTL_ANE_SUBMIT:
		return abi_major == ANE_ABI_MAJOR ? fake_submit(arg) :
			refuse("SUBMIT on ABI 2", request);
	case DRM_IOCTL_ANE_PROG_LOAD:
		return fake_prog_load(arg);
	case DRM_IOCTL_ANE_PROC_CREATE:
		((struct drm_ane_proc_create *)arg)->proc_id_out = 1;
		return 0;
	case DRM_IOCTL_ANE_EXEC:
		return abi_major == ANE_ABI_M2_MAJOR ? fake_exec(arg) :
			refuse("EXEC on ABI 1", request);
	}
	return refuse("unknown ioctl", request);
}

/* mmap_fail: 0 runs init, exec and free; N makes the Nth BO mmap fail, so
 * init must fail and free every BO it created. */
static void run_case(const char *name, int abi, const char *path,
		     int mmap_fail)
{
	int before = failures;
	struct ane_nn *nn;
	uint32_t h;

	abi_major = abi;
	mmap_countdown = mmap_fail;
	nn = ane_init(path);
	if (mmap_fail && nn) {
		printf("FAIL %s: init passed with a failed mmap\n", name);
		failures++;
		ane_free(nn);
	} else if (!mmap_fail && !nn) {
		printf("FAIL %s: init failed\n", name);
		failures++;
	} else if (nn) {
		if (ane_exec(nn)) {
			printf("FAIL %s: exec failed\n", name);
			failures++;
		}
		ane_free(nn);
	}
	if (mmap_countdown) {
		printf("FAIL %s: mmap %d not reached\n", name, mmap_fail);
		failures++;
	}
	for (h = 1; h <= next_handle; h++) {
		if (live[h]) {
			printf("FAIL %s: BO handle %u not freed\n", name, h);
			failures++;
			live[h] = 0;
		}
	}
	printf("  [%s] %s\n", failures == before ? "ok" : "FAIL", name);
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "../fixtures/h14-anec";
	char path[512];

	/* One stream, so libane's expected mmap errors print in order, above the
	 * scenario that injected them, under any log collector (CI keeps stdout
	 * and stderr apart and merges them out of order). */
	setvbuf(stdout, NULL, _IOLBF, 0);
	dup2(STDOUT_FILENO, STDERR_FILENO);
	snprintf(path, sizeof(path), "%s/add/program-0.anec", dir);
	run_case("ABI 2: load, exec and free", ANE_ABI_M2_MAJOR, path, 0);
	run_case("ABI 2: first BO mmap fails", ANE_ABI_M2_MAJOR, path, 1);
	run_case("ABI 2: third BO mmap fails", ANE_ABI_M2_MAJOR, path, 3);
	run_case("ABI 1: load, submit and free", ANE_ABI_MAJOR, path, 0);
	run_case("ABI 1: third BO mmap fails", ANE_ABI_MAJOR, path, 3);
	printf(failures ? "IOCTL-CHECK FAIL\n" : "IOCTL-CHECK PASS\n");
	return failures != 0;
}
