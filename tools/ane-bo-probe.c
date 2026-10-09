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

static uint64_t bo_total(void)
{
	char path[128];
	FILE *fp;
	unsigned long long v = 0;

	snprintf(path, sizeof(path),
		 "/sys/module/ane_t6021/parameters/bo_total_bytes");
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

int main(int argc, char **argv)
{
	const char *sizes_arg = "256,240,224,208,192,176,160,144,128,112,96,"
				"80,64,48,40,32,24,16,12,8,4,2,1";
	uint64_t before, size, offset;
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
				if (bo_total() != before && r == repeat - 1) {
					printf("NOTE bo_total moved %llu -> "
					       "%llu during the probe\n",
					       (unsigned long long)before,
					       (unsigned long long)bo_total());
				}
				printf("r%d %4llu MiB ok (handle off "
				       "%#llx)\n", r,
				       (unsigned long long)(size >> 20),
				       (unsigned long long)offset);
			} else {
				printf("r%d %4llu MiB fail (%s)\n", r,
				       (unsigned long long)(size >> 20),
				       strerror(errno));
			}
		}
		free(sizes);
		printf("r%d largest=%d MiB\n", r, largest);
	}
	if (bo_total() != before) {
		printf("LEAK net bo_total delta %lld bytes\n",
		       (long long)(bo_total() - before));
		return 1;
	}
	printf("PROBE DONE largest=%d MiB net-delta=0\n", largest);
	return 0;
}
