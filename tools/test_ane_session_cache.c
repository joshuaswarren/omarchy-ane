// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

/*
// test_ane_session_cache.c — host test for the ane-session LOAD cache:
// a second LOAD of the same (anec bytes, ports bytes) under any name
// must do NO device work at all (no BO_INIT, no PROG_LOAD), and FREE
// must only drop the name binding. Drives the real ane_session_main
// over a command file against a minimal fake DRM node, counting the
// BO_INITs it saw.
//
// Mutation: a session whose LOAD always re-inits (session_key_find
// returning NULL) fails the counts.
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
#include <sys/types.h>
#include <unistd.h>

#include <ane_accel.h>

#define ANE_SESSION_TEST
#include "ane-session.c"

/* ---- fake DRM node: counts BO_INITs, answers the ioctls libane uses ---- */

#define FAKE_NODE "/dev/accel/accel0"

static int fake_fd = -1;
static unsigned n_bo_inits;
static int failures;

static void check(int cond, const char *what)
{
	if (!cond) {
		printf("FAIL %s\n", what);
		failures++;
	}
}

int __real_open(const char *path, int flags, ...);
int __real_ioctl(int fd, unsigned long request, ...);
void *__real_mmap(void *addr, size_t len, int prot, int flags, int fd,
		  off_t off);

static int refuse(const char *what)
{
	printf("FAIL fake: %s refused\n", what);
	failures++;
	errno = EINVAL;
	return -1;
}

int __wrap_open(const char *path, int flags, ...)
{
	mode_t mode = 0;

	if (flags & O_CREAT) {
		va_list ap;

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
	return __real_mmap(NULL, len, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1,
			   0);
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
		v->date_len = 0;
		v->desc_len = 0;
		v->version_major = ANE_ABI_M2_MAJOR;
		v->version_minor = 0;
		v->version_patchlevel = 0;
		return 0;
	}
	case DRM_IOCTL_ANE_BO_INIT: {
		struct drm_ane_bo_init *a = arg;
		static uint32_t next_handle;

		if (!a->size)
			return refuse("BO_INIT size 0");
		a->handle = ++next_handle;
		a->offset = (uint64_t)next_handle << 32;
		n_bo_inits++;
		return 0;
	}
	case DRM_IOCTL_ANE_BO_FREE:
		return 0;
	case DRM_IOCTL_ANE_PROG_LOAD:
		((struct drm_ane_prog_load *)arg)->prog_id_out = 1;
		return 0;
	case DRM_IOCTL_ANE_PROC_CREATE:
		((struct drm_ane_proc_create *)arg)->proc_id_out = 1;
		return 0;
	case DRM_IOCTL_ANE_EXEC:
		return 0;
	}
	return refuse("unknown ioctl");
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

/* ---- the test ---- */

static unsigned count_sub(const char *hay, const char *needle)
{
	unsigned n = 0;
	const char *p = hay;

	while ((p = strstr(p, needle))) {
		n++;
		p++;
	}
	return n;
}

int main(void)
{
	char cmdpath[] = "/tmp/test_sess_cmds-XXXXXX";
	char outpath[] = "/tmp/test_sess_out-XXXXXX";
	char lockpath[] = "/tmp/test_sess_lock-XXXXXX";
	const char *anec = "../fixtures/h14-anec/add/program-0.anec";
	const char *ports = "../tests/fixtures/prog000-ports.json";
	char *argv[3];
	unsigned loads;
	int save_out, save_in;
	FILE *fp;
	long out_len;
	char *out;
	int fd, rc;
	int save_rc;

	save_in = dup(0);
	save_out = dup(1);
	fd = mkstemp(cmdpath);
	fp = fd < 0 ? NULL : fdopen(fd, "w");
	if (fp) {
		fprintf(fp, "LOAD a %s %s\n"
			    "LOAD b %s %s\n"
			    "FREE a\n"
			    "LOAD c %s %s\n"
			    "QUIT\n",
			anec, ports, anec, ports, anec, ports);
		fclose(fp);
	}
	mkstemp(outpath);
	mkstemp(lockpath);
	if (freopen(cmdpath, "r", stdin) != stdin ||
	    freopen(outpath, "w", stdout) != stdout) {
		printf("FAIL session-cache: cannot redirect streams\n");
		return 1;
	}
	argv[0] = (char *)"ane-session";
	argv[1] = (char *)"--lock";
	argv[2] = lockpath;
	rc = ane_session_main(3, argv);
	fflush(stdout);
	dup2(save_out, 1);
	dup2(save_in, 0);
	close(save_out);
	close(save_in);
	save_rc = rc;

	fp = fopen(outpath, "rb");
	fseek(fp, 0, SEEK_END);
	out_len = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	out = malloc((size_t)out_len + 1);
	if (fread(out, 1, (size_t)out_len, fp) != (size_t)out_len)
		out_len = 0;
	out[out_len] = 0;
	fclose(fp);

	check(save_rc == 0, "session exit 0");
	loads = count_sub(out, "OK LOAD");
	check(loads == 3, "three LOADs answered OK");
	check(!count_sub(out, "ERR"), "no ERR replies");

	/* Device work happened for the FIRST key only: one program's six
	 * section BO_INITs plus its nine io BO_INITs (the prog000 table);
	 * the two cached LOADs added none. */
	check(n_bo_inits == 15,
	      "exactly one program's 15 BO_INITs for three LOADs of one key");

	printf(failures ? "SESSION-CACHE FAIL\n" : "SESSION-CACHE PASS\n");
	unlink(cmdpath);
	unlink(outpath);
	unlink(lockpath);
	free(out);
	return failures != 0;
}
