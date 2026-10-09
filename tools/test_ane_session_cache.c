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

/* Run ane_session_main on a command file with the fake node; returns
 * the exit code and the reply text (caller frees). */
static int run_session(const char *lockpath, const char *cmds,
		       const char *cmdpath, const char *outpath,
		       unsigned max_progs, char **out_p)
{
	char *argv[5];
	int save_in = dup(0);
	int save_out = dup(1);
	int rc;
	int argc = 3;
	FILE *fp = fopen(cmdpath, "w");
	long out_len;

	if (!fp) {
		*out_p = NULL;
		return -1;
	}
	fputs(cmds, fp);
	fclose(fp);
	fp = fopen(outpath, "w");
	fclose(fp);
	if (freopen(cmdpath, "r", stdin) != stdin ||
	    freopen(outpath, "w", stdout) != stdout) {
		*out_p = NULL;
		return -1;
	}
	argv[0] = (char *)"ane-session";
	argv[1] = (char *)"--lock";
	argv[2] = (char *)lockpath;
	if (max_progs) {
		argv[argc++] = (char *)"--max-progs";
		argv[argc++] = (char *)"2";
	}
	rc = ane_session_main(argc, argv);
	fflush(stdout);
	dup2(save_out, 1);
	dup2(save_in, 0);
	close(save_out);
	close(save_in);
	fp = fopen(outpath, "rb");
	fseek(fp, 0, SEEK_END);
	out_len = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	*out_p = malloc((size_t)out_len + 1);
	if (fread(*out_p, 1, (size_t)out_len, fp) != (size_t)out_len)
		out_len = 0;
	(*out_p)[out_len] = 0;
	fclose(fp);
	return rc;
}

int main(void)
{
	char cmdpath[] = "/tmp/test_sess_cmds-XXXXXX";
	char outpath[] = "/tmp/test_sess_out-XXXXXX";
	char lockpath[] = "/tmp/test_sess_lock-XXXXXX";
	char anec2path[] = "/tmp/test_sess_anec2-XXXXXX";
	const char *anec = "../fixtures/h14-anec/add/program-0.anec";
	const char *ports = "../tests/fixtures/prog000-ports.json";
	char *out = NULL;
	int rc;
	unsigned inits_before;
	FILE *fp;
	int fd = mkstemp(anec2path);

	if (fd < 0) {
		printf("FAIL session-cache: cannot create the second anec\n");
		return 1;
	}
	fp = fdopen(fd, "wb");

	/* A byte-different copy of the fixture: a second cache key. */
	if (fp) {
		char buf[4096];
		size_t got;
		FILE *in = fopen(anec, "rb");

		while (in && (got = fread(buf, 1, sizeof(buf), in)) > 0) {
			buf[got - 1] ^= 0x5a;
			fwrite(buf, 1, got, fp);
		}
		if (in) {
			fclose(in);
		}
		fclose(fp);
		fp = NULL;
	}
	{
		char *tmp[3] = { cmdpath, outpath, lockpath };
		int i;

		for (i = 0; i < 3; i++) {
			fd = mkstemp(tmp[i]);
			if (fd < 0) {
				printf("FAIL session-cache: cannot create a temp file\n");
				return 1;
			}
			close(fd);
		}
	}

	/* Scenario 1: three LOADs of one key across a FREE -- one
	 * program's worth of device work, then none. */
	{
		size_t clen = strlen("LOAD a A P\nLOAD b A P\nFREE a\n"
				     "LOAD c A P\nQUIT\n") +
			      3 * strlen(anec) + 3 * strlen(ports) + 1;
		char *cmds = malloc(clen);

		snprintf(cmds, clen,
			 "LOAD a %s %s\nLOAD b %s %s\nFREE a\n"
			 "LOAD c %s %s\nQUIT\n",
			 anec, ports, anec, ports, anec, ports);
		rc = run_session(lockpath, cmds, cmdpath, outpath, 0, &out);
		free(cmds);
	}
	check(rc == 0, "scenario 1: session exit 0");
	check(count_sub(out, "OK LOAD") == 3,
	      "scenario 1: three LOADs answered OK");
	check(!count_sub(out, "ERR"), "scenario 1: no ERR replies");
	check(n_bo_inits == 15,
	      "scenario 1: 15 BO_INITs for three LOADs of one key");
	free(out);

	/* Scenario 2 (--max-progs 2): FREEd names keep their keys, so a
	 * THIRD distinct key must hit the cache bound with
	 * key-cache-full and no device work. */
	inits_before = n_bo_inits;
	{
		char anec3path[] = "/tmp/test_sess_anec3-XXXXXX";
		size_t clen;
		char *cmds;
		FILE *in = fopen(anec, "rb");

		fp = fopen(anec3path, "wb");
		if (in && fp) {
			char buf[4096];
			size_t got;

			while ((got = fread(buf, 1, sizeof(buf), in)) > 0) {
				buf[0] ^= 0xa5;
				fwrite(buf, 1, got, fp);
			}
		}
		if (in) {
			fclose(in);
		}
		if (fp) {
			fclose(fp);
		}
		clen = strlen("LOAD a A P\nLOAD b B P\nFREE a\nFREE b\n"
			      "LOAD c C P\nQUIT\n") +
		       2 * strlen(anec) + strlen(anec2path) +
		       strlen(anec3path) + 3 * strlen(ports) + 1;
		cmds = malloc(clen);
		snprintf(cmds, clen,
			 "LOAD a %s %s\nLOAD b %s %s\nFREE a\nFREE b\n"
			 "LOAD c %s %s\nQUIT\n",
			 anec, ports, anec2path, ports, anec3path, ports);
		rc = run_session(lockpath, cmds, cmdpath, outpath, 2, &out);
		free(cmds);
		unlink(anec3path);
	}
	check(rc == 1, "scenario 2: session exit 1 (failed LOAD)");
	check(count_sub(out, "key-cache-full") == 1,
	      "scenario 2: the third distinct key refused key-cache-full");
	check(count_sub(out, "device-open-failed") == 0,
	      "scenario 2: the bound refused BEFORE the device");
	/* Two new keys loaded (15 each); the refused third added none. */
	check(n_bo_inits - inits_before == 30,
	      "scenario 2: only the two new keys did BO_INITs");

	printf(failures ? "SESSION-CACHE FAIL\n" : "SESSION-CACHE PASS\n");
	unlink(cmdpath);
	unlink(outpath);
	unlink(lockpath);
	unlink(anec2path);
	free(out);
	return failures != 0;
}
