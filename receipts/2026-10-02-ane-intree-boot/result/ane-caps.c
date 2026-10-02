/* InTreeBoot: DRM_IOCTL_ANE_GET_CAPS probe + ABI-2 pad negative checks.
 * Build: gcc -I. -I/usr/include/drm -o ane-caps ane-caps.c
 * Header: include/uapi/drm/ane_accel.h from joshuaswarren/linux ane-driver-aurora f088ca5c. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "drm.h"
#include "ane_accel.h"

static int fd;

static const char *tryioctl(unsigned long req, void *arg, const char *what)
{
	int rc = ioctl(fd, req, arg);
	if (rc == 0)
		printf("%s: rc=0\n", what);
	else
		printf("%s: rc=%d errno=%d (%s)\n", what, rc, errno, strerror(errno));
	return rc == 0 ? "ok" : strerror(errno);
}

int main(int argc, char **argv)
{
	struct drm_ane_get_caps caps = {0};
	struct drm_ane_bo_init bi = {0};
	struct drm_ane_bo_free bf = {0};
	struct drm_ane_prog_load pl = {0};
	const char *path = argc > 1 ? argv[1] : "/dev/accel/accel0";
	const char *r;

	fd = open(path, O_RDWR);
	if (fd < 0) {
		perror(path);
		return 2;
	}

	r = tryioctl(DRM_IOCTL_ANE_GET_CAPS, &caps, "GET_CAPS flags=0");
	if (strcmp(r, "ok"))
		return 1;
	printf("GET_CAPS: size=%u abi_version=%u chip_family=%u section_size=%u bind_size=%u exec_io_size=%u\n",
	       caps.size, caps.abi_version, caps.chip_family, caps.section_size, caps.bind_size,
	       caps.exec_io_size);

	memset(&caps, 0, sizeof(caps));
	caps.flags = 1;
	tryioctl(DRM_IOCTL_ANE_GET_CAPS, &caps, "GET_CAPS flags=1 (want EINVAL)");

	bi.size = 4096;
	r = tryioctl(DRM_IOCTL_ANE_BO_INIT, &bi, "BO_INIT 4096");
	if (strcmp(r, "ok"))
		return 1;
	printf("BO_INIT: handle=%u offset=%llu\n", bi.handle, (unsigned long long)bi.offset);

	bf.handle = bi.handle;
	bf.pad = 1;
	tryioctl(DRM_IOCTL_ANE_BO_FREE, &bf, "BO_FREE pad=1 (want EINVAL)");

	bf.pad = 0;
	r = tryioctl(DRM_IOCTL_ANE_BO_FREE, &bf, "BO_FREE pad=0");
	if (strcmp(r, "ok"))
		printf("NOTE: handle was not freed by the pad=1 call (or double free rejected)\n");

	pl.pad = 1;
	tryioctl(DRM_IOCTL_ANE_PROG_LOAD, &pl, "PROG_LOAD pad=1 (want EINVAL)");
	close(fd);
	return 0;
}
