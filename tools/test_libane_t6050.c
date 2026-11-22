// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

/*
// T6050 (M5 Max) enumeration through the T6021-family libane path, host-only.
//
// The fake node answers DRM_IOCTL_VERSION as the aurora node does on the M5
// (name "ane", major 3) and the aurora GET_CAPS ioctl (abi_version 3,
// chip_family 17, size 32). Measured on m5max boot 7e6f417b, 2026-11-22.
//
// Cases:
//   1. by_chip(0x6050) on a V3/family-17 node  -> 0, family 17, abi 3
//   2. by_chip(0x6050) on a V3/family-13 node  -> -ENODEV (not a T6050)
//   3. by_chip(0x6021) on a V3 node            -> -EINVAL (no T6021 row yet)
//   4. by_chip(0x1234)                         -> -EINVAL (not in the table)
//   5. ane_device_info(1) with one node        -> -ENODEV
//   6. ane_device_info(0) on a V3 node         -> 0, abi 3
//
// usage: test_libane_t6050
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
#include <unistd.h>

#include <ane_accel.h>
#include "ane.h"

#define FAKE_NODE "/dev/accel/accel0"

static int fake_abi_major = 3;
static uint32_t fake_chip_family = ANE_CHIP_H17;
static int fake_fd = -1;
static int failures;

int __real_open(const char *path, int flags, ...);
int __real_ioctl(int fd, unsigned long request, ...);

static int fake_fd_p(int fd)
{
	return fd == fake_fd;
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
	/* Every other accel node is absent: enumeration sees one node. */
	errno = ENOENT;
	return -1;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
	void *arg = NULL;
	va_list ap;

	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);

	if (!fake_fd_p(fd)) {
		errno = EBADF;
		return -1;
	}

	if (request == DRM_IOCTL_VERSION) {
		/* The kernel fills every field on both the sizing and the
		 * buffer call; the gate reads version_major on the sizing
		 * call. */
		drm_version_t *v = arg;
		if (v->name && v->name_len >= 3) {
			memcpy(v->name, "ane", 3);
		}
		v->name_len = 3;
		v->date_len = 0;
		v->desc_len = 0;
		v->version_major = fake_abi_major;
		v->version_minor = 1;
		v->version_patchlevel = 0;
		return 0;
	}

	if (request == DRM_IOWR(0x40 + ANE_V3_GET_CAPS, struct ane_v3_get_caps)) {
		struct ane_v3_get_caps *caps = arg;
		if (caps->flags || caps->pad) {
			return -EINVAL;
		}
		caps->size = sizeof(*caps);
		caps->abi_version = fake_abi_major;
		caps->chip_family = fake_chip_family;
		caps->section_size = 16;
		caps->bind_size = 0;
		caps->exec_io_size = 16;
		return 0;
	}

	/* A V3 node answers GET_CAPS; anything else is not this driver. */
	errno = ENOTTY;
	return -1;
}

static void expect(int got, int want, const char *what)
{
	if (got != want) {
		printf("FAIL %s: got %d, want %d\n", what, got, want);
		failures++;
		return;
	}
	printf("ok   %s\n", what);
}

int main(void)
{
	struct ane_dev_info info = {};

	/* 1. the T6050 row matches the measured M5 node */
	info.fd = -1;
	expect(ane_device_info_by_chip(ANE_DEV_ID_T6050, &info), 0,
	       "by_chip T6050 on V3 family 17");
	if (info.chip_family != ANE_CHIP_H17 || info.abi_major != 3) {
		printf("FAIL info fields: family %u abi %d\n", info.chip_family,
		       info.abi_major);
		failures++;
	}
	close(info.fd);

	/* 2. a V3 node of another family is not a T6050 */
	fake_chip_family = 13;
	expect(ane_device_info_by_chip(ANE_DEV_ID_T6050, &info), -ENODEV,
	       "by_chip T6050 refuses family 13");
	fake_chip_family = ANE_CHIP_H17;

	/* 3. the table has no T6021 row on this path yet: its ABI-2 family id
	 * is not in the aurora GET_CAPS enum, so the row cannot be written
	 * honestly. Refusal is -EINVAL from the table lookup. */
	expect(ane_device_info_by_chip(ANE_DEV_ID_T6021, &info), -EINVAL,
	       "by_chip T6021 (no row yet)");

	/* 4. a chip outside the table is refused without touching nodes */
	expect(ane_device_info_by_chip(0x1234, &info), -EINVAL,
	       "by_chip unknown chip");

	/* 5. index 1 with one node */
	expect(ane_device_info(1, &info), -ENODEV, "device_info index 1");

	/* 6. plain index enumeration reaches the V3 node */
	expect(ane_device_info(0, &info), 0, "device_info index 0");
	if (info.abi_major != 3) {
		printf("FAIL device_info abi %d\n", info.abi_major);
		failures++;
	}
	close(info.fd);

	if (failures) {
		printf("%d failure(s)\n", failures);
		return 1;
	}
	printf("all T6050 enumeration cases pass\n");
	return 0;
}
