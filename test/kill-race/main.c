// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren <816217+joshuaswarren@users.noreply.github.com> */

/*
 * Kill-race repro for the SIGKILL-mid-submit lifecycle bug.
 *
 * Per iteration, exactly the incident shape from m1-test-host 2026-09-25
 * (stale DART PTE at 0x4000, dart_init_pte -EEXIST WARN, BO_INIT
 * broken until module reload):
 *
 *   child:  open, BO_INIT large BOs, SUBMIT (long-running), ...
 *   parent: SIGKILL the child while the submit is in flight,
 *   parent: reopen, BO_INIT large BOs again - every one must succeed.
 *
 * Any -EEXIST/-ENOSPC/ENOMEM on the reopen path is the poison: the
 * allocator handed out a range whose DART PTEs never went away.
 * Exit 0 only when every iteration's reopen allocations are clean.
 *
 * Runs without a compiled model: the submitted task image is
 * garbage-but-plausible (bind-style selector + DMA-config records),
 * which is exactly what makes the submit stay in flight while the
 * engine chews or faults through containment.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <linux/types.h>

/* Minimal drm uapi compat so the test builds on hosts without libdrm
 * headers; matches uapi/drm/drm.h definitions. */
#ifndef DRM_IOCTL_BASE
#define DRM_IOCTL_BASE		'd'
#define DRM_COMMAND_BASE	0x40
#define DRM_IOWR(nr, type)	_IOWR(DRM_IOCTL_BASE, nr, type)
#endif
#include "drm/ane_accel.h"

#define ANE_DEV		"/dev/accel/accel0"

#define CMD_BO_SIZE	(8UL << 20)	/* 8 MiB task buffer */
#define BIG_BO_SIZE	(256UL << 20)	/* 256 MiB payload BO */
#define BTSP_BO_SIZE	(512UL << 10)	/* >= td_size */

#define TSK_SIZE	0x1000UL	/* task image bytes, < CMD_BO_SIZE */
#define TD_SIZE		0x40000U	/* max driver-allowed descriptor size */
#define TD_COUNT	0xffffU		/* max descriptor count: keeps the
					 * submit in flight longest */

#define KILL_DELAY_US	200000		/* kill 200 ms into the submit */
#define DRAIN_GRACE_S	5		/* submit retires within ~1 s poll
					 * plus bounded containment */

static int bo_init(int fd, unsigned long size, uint32_t *handle,
		   unsigned long *offset)
{
	struct drm_ane_bo_init args;
	int ret;

	memset(&args, 0, sizeof(args));
	args.size = size;

	ret = ioctl(fd, DRM_IOCTL_ANE_BO_INIT, &args);
	if (ret < 0)
		return -errno;

	*handle = args.handle;
	*offset = args.offset;
	return 0;
}

static int bo_free(int fd, uint32_t handle)
{
	struct drm_ane_bo_free args;

	memset(&args, 0, sizeof(args));
	args.handle = handle;
	return ioctl(fd, DRM_IOCTL_ANE_BO_FREE, &args) < 0 ? -1 : 0;
}

static int do_submit(int fd, uint32_t cmd_handle, uint32_t btsp_handle,
		     unsigned long cmd_map_offset)
{
	struct drm_ane_submit args;
	uint32_t *task;
	int ret;

	task = mmap(NULL, TSK_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		    cmd_map_offset);
	if (task == MAP_FAILED)
		return -1;

	/* bind-style minimal image: selector word at byte 32, then
	 * register records (address, value pairs) for the three DMA
	 * config registers. Contents are deliberately inert garbage:
	 * the engine grinds or faults, which is what keeps the submit
	 * in flight while the kill lands. */
	memset(task, 0, TSK_SIZE);
	((volatile uint32_t *)task)[8] = 0x1u;			/* selector */
	((volatile uint32_t *)task)[(0x28) / 4] = 0x13800u;	/* src1 cfg reg */
	((volatile uint32_t *)task)[(0x28) / 4 + 1] = 0x00033881u;
	((volatile uint32_t *)task)[(0x30) / 4] = 0x13804u;	/* src2 cfg reg */
	((volatile uint32_t *)task)[(0x30) / 4 + 1] = 0x00033881u;
	((volatile uint32_t *)task)[(0x38) / 4] = 0x17800u;	/* dst cfg reg */
	((volatile uint32_t *)task)[(0x38) / 4 + 1] = 0x040000c1u;

	memset(&args, 0, sizeof(args));
	args.tsk_size = TSK_SIZE;
	args.td_count = TD_COUNT;
	args.td_size = TD_SIZE;
	args.handles[0] = cmd_handle;	/* CMD_BUF_BDX; krn derives from it */
	args.btsp_handle = btsp_handle;

	ret = ioctl(fd, DRM_IOCTL_ANE_SUBMIT, &args);

	munmap(task, TSK_SIZE);
	/* Any terminal result is fine (-EIO/-ECANCELED expected for a
	 * garbage task); only the reopen phase below is judged. */
	return ret;
}

/* child: holds the BOs and dies mid-submit */
static void run_child(int ready_pipe)
{
	unsigned long cmd_off = 0, big_off = 0, btsp_off = 0;
	uint32_t cmd = 0, big = 0, btsp = 0;
	int fd;
	char ready = 1;

	fd = open(ANE_DEV, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		_exit(2);

	if (bo_init(fd, CMD_BO_SIZE, &cmd, &cmd_off) ||
	    bo_init(fd, BIG_BO_SIZE, &big, &big_off) ||
	    bo_init(fd, BTSP_BO_SIZE, &btsp, &btsp_off))
		_exit(3);

	if (write(ready_pipe, &ready, 1) != 1)
		_exit(4);

	/* parent kills us somewhere inside this */
	do_submit(fd, cmd, btsp, cmd_off);
	_exit(0);
}

int main(int argc, char **argv)
{
	int iterations = argc > 1 ? atoi(argv[1]) : 1;
	int failures = 0;
	int it;

	if (access(ANE_DEV, F_OK) != 0) {
		printf("SKIP: no ANE device at " ANE_DEV "\n");
		return 77;
	}

	for (it = 1; it <= iterations; it++) {
		unsigned long big_off = 0, check_off = 0;
		uint32_t big_handle = 0;
		int pipefd[2];
		pid_t pid;
		int status;
		char ack;

		if (pipe(pipefd))
			return 2;

		pid = fork();
		if (pid < 0)
			return 2;
		if (pid == 0) {
			close(pipefd[0]);
			run_child(pipefd[1]);
		}
		close(pipefd[1]);

		if (read(pipefd[0], &ack, 1) != 1) {
			printf("iter %d: FAIL child never readied\n", it);
			failures++;
			kill(pid, SIGKILL);
			waitpid(pid, NULL, 0);
			close(pipefd[0]);
			continue;
		}
		close(pipefd[0]);

		usleep(KILL_DELAY_US);
		if (kill(pid, SIGKILL) || waitpid(pid, &status, 0) < 0) {
			printf("iter %d: FAIL kill/wait\n", it);
			failures++;
			continue;
		}

		/* let the in-flight submit retire before reopening */
		sleep(DRAIN_GRACE_S);

		{
			int fd = open(ANE_DEV, O_RDWR | O_CLOEXEC);
			uint32_t h = 0;
			unsigned long off = 0;
			int k, bad = 0;

			if (fd < 0) {
				printf("iter %d: FAIL reopen\n", it);
				failures++;
				continue;
			}

			/* the poison test: fresh large BOs must map clean
			 * now that the killed holder is gone */
			for (k = 0; k < 4 && !bad; k++) {
				if (bo_init(fd, BIG_BO_SIZE, &h, &off)) {
					printf("iter %d: FAIL bo_init #%d after kill (stale PTE / poisoned range?)\n",
					       it, k);
					bad = 1;
					break;
				}
				big_handle = h;
				big_off = off;
				bo_free(fd, big_handle);
				big_handle = 0;
			}

			/* and one that must persist across a short hold */
			if (!bad && bo_init(fd, BIG_BO_SIZE, &h, &off)) {
				printf("iter %d: FAIL final bo_init after kill\n", it);
				bad = 1;
			} else if (!bad) {
				big_handle = h;
				big_off = off;
			}
			check_off = big_off;
			if (big_handle)
				bo_free(fd, big_handle);

			close(fd);

			if (bad) {
				failures++;
				continue;
			}
			printf("iter %d: ok kill + reopen + bo_init (last range base %#lx)\n",
			       it, check_off);
		}
	}

	if (failures) {
		printf("KILL-RACE: FAIL (%d/%d iterations poisoned)\n",
		       failures, iterations);
		return 1;
	}
	printf("KILL-RACE: PASS (%d iterations, no poisoned ranges)\n",
	       iterations);
	return 0;
}
