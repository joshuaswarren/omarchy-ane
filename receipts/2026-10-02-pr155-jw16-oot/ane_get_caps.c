// SPDX-License-Identifier: GPL-2.0-only
/*
 * GET_CAPS probe for the PR155 ane module (H13, ABI version 1) on jw16.
 *
 * Build (build.sh does this too):
 *   cc -O2 -Wall -I uapi -I /usr/include/drm ane_get_caps.c -o ane_get_caps
 *
 * Pass: the accel node opens, the DRM driver is "ane" with version major 1
 * (ABI 1), and GET_CAPS reports abi_version 1, chip_family 13 (H13) with all
 * array entry sizes 0 (ABI 1 has no PROG_LOAD/EXEC). A nonzero flags field
 * must be refused with -EINVAL.
 *
 * Per include/uapi/drm/ane_accel.h at the PR commit: every pad/reserved
 * field is zero, GET_CAPS is number 0x00 on DRM_COMMAND_BASE.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <drm/drm.h>
#include <drm/ane_accel.h> /* the branch uapi copy in ane/uapi/drm/ */

static int fails;

int main(int argc, char **argv)
{
	const char *node = argc > 1 ? argv[1] : "/dev/accel/accel0";
	char name[64] = { 0 };
	struct drm_version v = { 0 };
	struct drm_ane_get_caps caps = { 0 };
	int fd;

	fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		printf("FAIL: open %s: %s\n", node, strerror(errno));
		return 1;
	}

	/* DRM version: driver name and ABI-version major. */
	v.name = name;
	v.name_len = sizeof(name) - 1;
	if (ioctl(fd, DRM_IOCTL_VERSION, &v)) {
		printf("FAIL: DRM_IOCTL_VERSION: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	printf("DRM version: driver=%s major=%u minor=%u\n",
	       name, v.version_major, v.version_minor);
	fails += !(strcmp(name, "ane") == 0);
	fails += !(v.version_major == DRM_ANE_ABI_V1);

	/* GET_CAPS: ABI 1 expects abi_version 1, chip_family 13, sizes 0. */
	if (ioctl(fd, DRM_IOCTL_ANE_GET_CAPS, &caps)) {
		printf("FAIL: DRM_IOCTL_ANE_GET_CAPS: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	printf("GET_CAPS: size=%u abi_version=%u chip_family=%u "
	       "section_size=%u bind_size=%u exec_io_size=%u pad=%u\n",
	       caps.size, caps.abi_version, caps.chip_family,
	       caps.section_size, caps.bind_size, caps.exec_io_size,
	       caps.pad);
	fails += !(caps.abi_version == DRM_ANE_ABI_V1);
	fails += !(caps.chip_family == DRM_ANE_CHIP_H13);
	fails += !(caps.section_size == 0);
	fails += !(caps.bind_size == 0);
	fails += !(caps.exec_io_size == 0);
	fails += !(caps.size > 0 && caps.size <= sizeof(caps));
	fails += !(caps.flags == 0);
	fails += !(caps.pad == 0);

	/* Negative: flags must be zero; the driver refuses nonzero. */
	memset(&caps, 0, sizeof(caps));
	caps.flags = 1;
	if (ioctl(fd, DRM_IOCTL_ANE_GET_CAPS, &caps) == 0) {
		printf("FAIL: GET_CAPS accepted nonzero flags\n");
		fails++;
	} else if (errno != EINVAL) {
		printf("FAIL: GET_CAPS nonzero flags errno=%s (want EINVAL)\n",
		       strerror(errno));
		fails++;
	} else {
		printf("ok: GET_CAPS nonzero flags refused with EINVAL\n");
	}

	close(fd);
	if (fails) {
		printf("ANE_GET_CAPS: FAIL (%d check%s)\n",
		       fails, fails == 1 ? "" : "s");
		return 1;
	}
	printf("ANE_GET_CAPS: PASS driver=ane drm_major=1 abi=1 family=13 "
	       "sizes=0/0/0\n");
	return 0;
}
