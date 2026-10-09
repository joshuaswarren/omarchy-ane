/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Joshua Warren */

#include <drm.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <ane_accel.h>

/*
// ane-bo-probe: largest allocatable BO size, by BO_INIT/BO_FREE only.
//
// Reads the dma32 IOVA headroom of the ane_t6021 device without sending
// a single firmware command: no PROG_LOAD, no PROC_CREATE, no EXEC, no
// SUBMIT. BO_INIT/BO_FREE touch only the kernel allocator (the driver
// refuses nothing else here), so the tool is safe next to a live
// workload as long as the gpu-turn lock is held: every allocation is
// freed before the next probe step and the counter returns to its
// starting value.
//
//   ane-bo-probe [--dev N] [--sizes MiB,...] [--repeat N]
//
// Default sizes walk down from 256 MiB; the tool prints one line per
// step, "ok"/"fail" per size, and the largest size that allocated.
// A size that allocates but whose bo_total_bytes delta is not returned
// by BO_FREE is reported as LEAK (it never should).
*/

#define PROBE_NODE_FMT "/dev/accel/accel%d"

static int fd = -1;

static int bo_init(uint64_t size, uint32_t *handle, uint64_t *offset)
{
	struct drm_ane_bo_init args = { .size = size };

	if (ioctl(fd, DRM_IOCTL_ANE_BO_INIT, &args) < 0) {
		return -errno ? -1 : -1;
	}
	*handle = args.handle;
	*offset = args.offset;
	return 0;
}

static int bo_free(uint32_t handle)
{
	struct drm_ane_bo_free args = { .handle = handle };

	return ioctl(fd, DRM_IOCTL_ANE_BO_FREE, &args) < 0 ? -1 : 0;
}

static const char *sysfs_dir(void)
{
	/* Test hook: the host test points this at a fake parameter dir. */
	return getenv("ANE_BO_PROBE_SYSFS") ?
		       getenv("ANE_BO_PROBE_SYSFS") :
		       "/sys/module/ane_t6021/parameters";
}

static unsigned long long rd_param(const char *name)
{
	char path[256];
	FILE *fp;
	unsigned long long v = 0;

	snprintf(path, sizeof(path), "%s/%s", sysfs_dir(), name);
	fp = fopen(path, "r");
	if (!fp) {
		return 0;
	}
	if (fscanf(fp, "%llu", &v) != 1) {
		v = 0;
	}
	fclose(fp);
	return v;
}

static uint64_t bo_total(void)
{
	return rd_param("bo_total_bytes");
}

static uint64_t bo_pool(void)
{
	return rd_param("bo_pool_bytes");
}

static uint64_t pow2_mib(uint64_t mib)
{
	uint64_t c = 1;

	while (c < mib) {
		c <<= 1;
	}
	return c;
}

#ifdef ANE_BO_PROBE_TEST
int ane_bo_probe_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
	const char *sizes_arg = "256,240,224,208,192,176,160,144,128,112,96,"
				"80,64,48,40,32,24,16,12,8,4,2,1";
	uint64_t before, after, pool_before, pool_after, size, offset;
	long long delta_total, delta_pool;
	uint32_t handle;
	char path[64];
	char *tok, *end, *sizes, *save;
	int repeat = 1, dev = 0, largest = 0, r;
	drm_version_t ver = { 0 };
	char name[8] = { 0 };

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--dev") && i + 1 < argc) {
			dev = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--sizes") && i + 1 < argc) {
			sizes_arg = argv[++i];
		} else if (!strcmp(argv[i], "--repeat") && i + 1 < argc) {
			repeat = atoi(argv[++i]);
		} else {
			fprintf(stderr,
				"usage: ane-bo-probe [--dev N] "
				"[--sizes MiB,...] [--repeat N]\n");
			return 2;
		}
	}
	snprintf(path, sizeof(path), PROBE_NODE_FMT, dev);
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror(path);
		return 2;
	}
	ver.name = name;
	ver.name_len = sizeof(name) - 1;
	if (ioctl(fd, DRM_IOCTL_VERSION, &ver) < 0 || strcmp(name, "ane")) {
		fprintf(stderr, "%s is not an ane device\n", path);
		return 2;
	}

	before = bo_total();
	pool_before = bo_pool();
	for (r = 0; r < repeat; r++) {
		largest = 0;
		sizes = strdup(sizes_arg);
		if (!sizes) {
			fprintf(stderr, "out of memory\n");
			return 2;
		}
		for (tok = strtok_r(sizes, ",", &save); tok;
		     tok = strtok_r(NULL, ",", &save)) {
			size = (uint64_t)strtoul(tok, &end, 10) << 20;
			if (!size) {
				continue;
			}
			if (!bo_init(size, &handle, &offset)) {
				if ((int)(size >> 20) > largest) {
					largest = (int)(size >> 20);
				}
				if (bo_free(handle)) {
					printf("LEAK size %llu MiB: "
					       "BO_FREE failed: %s\n",
					       (unsigned long long)(size >> 20),
					       strerror(errno));
					return 1;
				}
				printf("r%d %4llu MiB (iova class %3llu MiB)"
				       " ok\n", r,
				       (unsigned long long)(size >> 20),
				       (unsigned long long)pow2_mib(size >> 20));
			} else {
				printf("r%d %4llu MiB (iova class %3llu MiB)"
				       " fail (%s)\n", r,
				       (unsigned long long)(size >> 20),
				       (unsigned long long)pow2_mib(size >> 20),
				       strerror(errno));
			}
		}
		free(sizes);
		printf("r%d largest=%d MiB\n", r, largest);
	}
	pool_after = bo_pool();
	after = bo_total();
	printf("bo_pool_bytes %llu -> %llu\n",
	       (unsigned long long)pool_before,
	       (unsigned long long)pool_after);
	/* The driver PARKS freed BOs of >= bo_pool_min_kb: their bytes
	 * stay counted and their IOVA stays mapped (that is the reuse
	 * this probe wants to observe). A net bo_total move that equals
	 * the pool move is parking, not a leak; only a mismatch is. */
	delta_total = (long long)(after - before);
	delta_pool = (long long)(pool_after - pool_before);
	if (delta_total == 0 && delta_pool == 0) {
		printf("PROBE DONE largest=%d MiB net-delta=0\n", largest);
		return 0;
	}
	if (delta_total == delta_pool) {
		printf("PARKED net-delta=%lld (bo_pool_bytes moved by the "
		       "same amount; drain with bo_pool_max_mb=0 to "
		       "release)\n", delta_pool);
		return 0;
	}
	printf("LEAK net bo_total delta %lld with pool delta %lld\n",
	       delta_total, delta_pool);
	return 1;
}
