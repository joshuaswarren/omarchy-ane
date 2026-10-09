// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

/*
// test_ane_bo_probe.c — host test for ane-bo-probe's net check: the
// driver parks freed BOs (bytes stay counted in bo_total_bytes AND
// bo_pool_bytes), so "bo_total moved" is PARKING, not a leak. Only a
// bo_total move the pool does not match is a leak.
//
// The fake driver maintains the fake sysfs files the probe reads:
//   park mode: BO_FREE keeps bo_total and grows bo_pool  -> PARKED, rc 0
//   free mode: BO_FREE returns bo_total                  -> net 0, rc 0
//   leak mode: BO_FREE keeps bo_total, pool untouched    -> LEAK,  rc 1
*/

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
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <ane_accel.h>

#ifndef ANE_BO_PROBE_TEST
#define ANE_BO_PROBE_TEST
#endif
#include "ane-bo-probe.c"

#define FAKE_NODE "/dev/accel/accel0"

static int fake_fd = -1;
static int failures;
static uint64_t alloc_total;	/* fake bo_total_bytes */
static uint64_t pool_bytes;	/* fake bo_pool_bytes */
static uint64_t handle_size[256];
static const char *fake_mode = "park";

static void check(int cond, const char *what)
{
	if (!cond) {
		printf("FAIL %s\n", what);
		failures++;
	}
}

static void sysfs_set(const char *name, unsigned long long v)
{
	char path[256];
	FILE *fp;

	snprintf(path, sizeof(path), "%s/%s", getenv("ANE_BO_PROBE_SYSFS"),
		 name);
	fp = fopen(path, "w");
	if (fp) {
		fprintf(fp, "%llu\n", v);
		fclose(fp);
	}
}

int __real_open(const char *path, int flags, ...);
int __real_ioctl(int fd, unsigned long request, ...);
void *__real_mmap(void *addr, size_t len, int prot, int flags, int fd,
		  off_t off);

int __wrap_open(const char *path, int flags, ...)
{
	if (!strcmp(path, FAKE_NODE)) {
		fake_fd = __real_open("/dev/null", O_RDWR | O_CLOEXEC);
		return fake_fd;
	}
	return __real_open(path, flags);
}

void *__wrap_mmap(void *addr, size_t len, int prot, int flags, int fd,
		  off_t off)
{
	return __real_mmap(addr, len, prot, flags, fd, off);
}

static int fake_ioctl(int fd, unsigned long request, void *arg)
{
	if (fd != fake_fd) {
		return __real_ioctl(fd, request, arg);
	}
	switch (request) {
	case DRM_IOCTL_VERSION: {
		drm_version_t *v = arg;

		if (v->name && v->name_len >= 3) {
			memcpy(v->name, "ane", 3);
		}
		v->name_len = 3;
		v->version_major = ANE_ABI_M2_MAJOR;
		return 0;
	}
	case DRM_IOCTL_ANE_BO_INIT: {
		struct drm_ane_bo_init *a = arg;
		static uint32_t next_handle;

		a->handle = ++next_handle;
		a->offset = (uint64_t)a->handle << 32;
		handle_size[a->handle] = a->size;
		alloc_total += a->size;
		sysfs_set("bo_total_bytes", alloc_total);
		return 0;
	}
	case DRM_IOCTL_ANE_BO_FREE: {
		struct drm_ane_bo_free *a = arg;
		uint64_t pg = handle_size[a->handle];

		if (!strcmp(fake_mode, "park")) {
			/* The driver parks: bytes stay counted, the pool
			 * grows by the same amount. */
			pool_bytes += pg;
			sysfs_set("bo_pool_bytes", pool_bytes);
		} else if (!strcmp(fake_mode, "free")) {
			alloc_total -= pg;
			sysfs_set("bo_total_bytes", alloc_total);
		}
		/* leak mode: bo_total stays up, pool untouched. */
		return 0;
	}
	}
	return -1;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	void *arg;
	int ret;

	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);
	ret = fake_ioctl(fd, request, arg);
	if (ret < 0 && errno == 0)
		errno = EINVAL;
	return ret;
}

static int run_probe(char **out_p)
{
	char *argv[5];
	int save_out = dup(1);
	int rc;
	FILE *fp = fopen("/tmp/test_probe_out", "w");
	long len;

	if (!fp) {
		*out_p = NULL;
		return -1;
	}
	fflush(stdout);
	dup2(fileno(fp), 1);
	fclose(fp);
	argv[0] = (char *)"ane-bo-probe";
	argv[1] = (char *)"--sizes";
	argv[2] = (char *)"16";
	rc = ane_bo_probe_main(3, argv);
	fflush(stdout);
	dup2(save_out, 1);
	close(save_out);
	fp = fopen("/tmp/test_probe_out", "rb");
	fseek(fp, 0, SEEK_END);
	len = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	*out_p = malloc((size_t)len + 1);
	if (fread(*out_p, 1, (size_t)len, fp) != (size_t)len)
		len = 0;
	(*out_p)[len] = 0;
	fclose(fp);
	return rc;
}

static int failures;

static void scenario(const char *name, const char *mode, int want_rc,
		     const char *want_word)
{
	char *out = NULL;
	int rc;

	fake_mode = mode;
	alloc_total = 0;
	pool_bytes = 0;
	sysfs_set("bo_total_bytes", 0);
	sysfs_set("bo_pool_bytes", 0);
	rc = run_probe(&out);
	check(rc == want_rc, name);
	check(out && strstr(out, want_word), name);
	check(out && strstr(out, "bo_pool_bytes"), name);
	free(out);
}

int main(void)
{
	char sysfsdir[] = "/tmp/test_probe_sysfs-XXXXXX";

	mkdir(sysfsdir, 0755);
	setenv("ANE_BO_PROBE_SYSFS", sysfsdir, 1);
	setvbuf(stdout, NULL, _IONBF, 0);
	scenario("park mode: parking reported as PARKED, rc 0", "park", 0,
		 "PARKED");
	scenario("free mode: full return reported as net-delta=0, rc 0",
		 "free", 0, "net-delta=0");
	scenario("leak mode: unmatched bo_total move is a LEAK, rc 1",
		 "leak", 1, "LEAK");
	printf(failures ? "PROBE-NET FAIL\n" : "PROBE-NET PASS\n");
	return failures != 0;
}
