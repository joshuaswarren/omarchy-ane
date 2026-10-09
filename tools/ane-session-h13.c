/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Joshua Warren */

/* ane-session-h13: the ane-session resident protocol for the H13 (M1 family,
 * ABI-1) path. tools/ane-session.c is ABI-2 (T6021) only: it loads programs
 * through ane_m2_init_ports and moves surfaces with ane_m2_send/read over a
 * ports.json io table. H13 libane io is by channel index with sizes the
 * driver itself reports, so this variant keeps the same text protocol but
 * takes sizes from libane at LOAD time:
 *
 *   PING                          -> OK PONG
 *   LOAD <name> <anec>            -> OK LOAD <name> <n_in> <n_out> <in_bytes> <out_bytes>
 *                                | ERR LOAD <name> <reason>
 *   FREE <name>                   -> OK FREE <name> | ERR FREE <name> <reason>
 *   CALL <name> + <in_bytes> raw  -> OK CALL <name> <exec_us> + <out_bytes> raw
 *                                | ERR CALL <name> <reason>
 *   LOCK | UNLOCK                 -> OK LOCK | OK UNLOCK   (flock the lock file)
 *   QUIT                          -> OK QUIT, exit 0 (1 if a CALL failed)
 *
 * The queue scripts supply io packing shapes from the fixture's ports table
 * (h13-ports.json); this process is shape-agnostic and moves raw channel
 * bytes in libane index order (inputs 0..n_in-1, outputs 0..n_out-1).
 * Guards before any device open: program-count bound (ANE_SESSION_MAX_PROGS)
 * and duplicate-LOAD refusal, mirroring tools/ane-session.c.
 */

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#include "ane.h"

#define ANE_SESSION_MAX_PROGS 250

struct session_prog {
	char *name;
	struct ane_nn *nn;
	uint32_t n_in, n_out;
	uint64_t in_total, out_total;
};

struct session {
	struct session_prog progs[ANE_SESSION_MAX_PROGS];
	uint32_t nprogs;
	int lock_fd;
	int dev;
	int failed;
	const char *lock_path;
};

static void replyf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
}

static int read_stream(void *buf, uint64_t n)
{
	return n && fread(buf, 1, (size_t)n, stdin) != (size_t)n ? -1 : 0;
}

static struct session_prog *session_prog_find(struct session *s,
					      const char *name)
{
	uint32_t i;

	for (i = 0; i < ANE_SESSION_MAX_PROGS; i++) {
		if (s->progs[i].name && !strcmp(s->progs[i].name, name)) {
			return &s->progs[i];
		}
	}
	return NULL;
}

static void session_prog_free(struct session_prog *p)
{
	if (p->nn) {
		ane_free(p->nn);
	}
	free(p->name);
	memset(p, 0, sizeof(*p));
}

static int cmd_load(struct session *s, const char *name, const char *anec)
{
	struct session_prog *p = NULL;
	struct ane_nn *nn;
	uint64_t in_total = 0, out_total = 0;
	uint32_t i;

	if (session_prog_find(s, name)) {
		replyf("ERR LOAD %s already-loaded\n", name);
		s->failed = 1;
		return -1;
	}
	/* Guard: session program count, before any device open. */
	if (s->nprogs >= ANE_SESSION_MAX_PROGS) {
		replyf("ERR LOAD %s max-progs %u reached (refusing before "
		       "the device)\n", name, (unsigned)ANE_SESSION_MAX_PROGS);
		s->failed = 1;
		return -1;
	}
	nn = s->dev ? __ane_init(anec, s->dev) : ane_init(anec);
	if (!nn) {
		replyf("ERR LOAD %s device-open-failed\n", name);
		s->failed = 1;
		return -1;
	}
	for (i = 0; i < ane_src_count(nn); i++) {
		in_total += __ane_src_size(nn, i);
	}
	for (i = 0; i < ane_dst_count(nn); i++) {
		out_total += __ane_dst_size(nn, i);
	}
	if (!in_total || !out_total) {
		replyf("ERR LOAD %s empty-io (in %llu out %llu)\n", name,
		       (unsigned long long)in_total,
		       (unsigned long long)out_total);
		ane_free(nn);
		s->failed = 1;
		return -1;
	}
	for (i = 0; i < ANE_SESSION_MAX_PROGS; i++) {
		if (!s->progs[i].name) {
			p = &s->progs[i];
			break;
		}
	}
	p->name = strdup(name);
	if (!p->name) {
		replyf("ERR LOAD %s no-mem\n", name);
		ane_free(nn);
		s->failed = 1;
		return -1;
	}
	p->nn = nn;
	p->n_in = ane_src_count(nn);
	p->n_out = ane_dst_count(nn);
	p->in_total = in_total;
	p->out_total = out_total;
	s->nprogs++;
	replyf("OK LOAD %s %u %u %llu %llu\n", name, (unsigned)p->n_in,
	       (unsigned)p->n_out, (unsigned long long)in_total,
	       (unsigned long long)out_total);
	return 0;
}

static int cmd_free(struct session *s, const char *name)
{
	struct session_prog *p = session_prog_find(s, name);

	if (!p) {
		replyf("ERR FREE %s not-loaded\n", name);
		return -1;
	}
	session_prog_free(p);
	s->nprogs--;
	replyf("OK FREE %s\n", name);
	return 0;
}

static int cmd_call(struct session *s, const char *name)
{
	struct session_prog *p = session_prog_find(s, name);
	uint64_t off;
	uint32_t k;
	struct timespec t0, t1;
	uint8_t *buf;

	if (!p) {
		/* Payload length unknown for an unknown program: the stream
		 * is unrecoverable, fail the session. */
		replyf("ERR CALL %s not-loaded\n", name);
		s->failed = 1;
		return -1;
	}
	if (s->failed) {
		replyf("ERR CALL %s session-failed\n", name);
		return -1;
	}
	buf = malloc((size_t)p->in_total);
	if (!buf) {
		replyf("ERR CALL %s no-mem\n", name);
		s->failed = 1;
		return -1;
	}
	if (read_stream(buf, p->in_total)) {
		replyf("ERR CALL %s short-input\n", name);
		free(buf);
		s->failed = 1;
		return -1;
	}
	off = 0;
	for (k = 0; k < p->n_in; k++) {
		__ane_send(p->nn, buf + off, k);
		off += __ane_src_size(p->nn, k);
	}
	free(buf);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	if (ane_exec(p->nn) < 0) {
		replyf("ERR CALL %s exec-failed\n", name);
		s->failed = 1;
		return -1;
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	replyf("OK CALL %s %lld\n", name,
	       (long long)((t1.tv_sec - t0.tv_sec) * 1000000ll +
			   (t1.tv_nsec - t0.tv_nsec) / 1000));
	buf = malloc((size_t)p->out_total);
	if (!buf) {
		s->failed = 1;
		return -1;
	}
	off = 0;
	for (k = 0; k < p->n_out; k++) {
		__ane_read(p->nn, buf + off, k);
		off += __ane_dst_size(p->nn, k);
	}
	if (fwrite(buf, 1, (size_t)p->out_total, stdout) !=
	    (size_t)p->out_total) {
		free(buf);
		s->failed = 1;
		return -1;
	}
	fflush(stdout);
	free(buf);
	return 0;
}

static int cmd_lock(struct session *s)
{
	if (s->lock_fd < 0) {
		s->lock_fd = open(s->lock_path, O_RDWR | O_CREAT, 0666);
		if (s->lock_fd < 0) {
			replyf("ERR LOCK %s\n", strerror(errno));
			s->failed = 1;
			return -1;
		}
	}
	if (flock(s->lock_fd, LOCK_EX)) {
		replyf("ERR LOCK %s\n", strerror(errno));
		s->failed = 1;
		return -1;
	}
	replyf("OK LOCK\n");
	return 0;
}

static int cmd_unlock(struct session *s)
{
	if (s->lock_fd >= 0 && flock(s->lock_fd, LOCK_UN)) {
		replyf("ERR UNLOCK %s\n", strerror(errno));
		s->failed = 1;
		return -1;
	}
	replyf("OK UNLOCK\n");
	return 0;
}

static void session_free(struct session *s)
{
	uint32_t i;

	for (i = 0; i < ANE_SESSION_MAX_PROGS; i++) {
		if (s->progs[i].name) {
			session_prog_free(&s->progs[i]);
		}
	}
	if (s->lock_fd >= 0) {
		flock(s->lock_fd, LOCK_UN);
		close(s->lock_fd);
		s->lock_fd = -1;
	}
}

int main(int argc, char **argv)
{
	struct session s = { .lock_fd = -1, .dev = 0 };
	char *line = NULL;
	size_t line_cap = 0;
	ssize_t len;
	int i, ret = 0;

	s.lock_path = "/var/tmp/ane-run.lock";
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--dev") && i + 1 < argc) {
			s.dev = (int)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--lock") && i + 1 < argc) {
			s.lock_path = argv[++i];
		} else {
			fprintf(stderr, "usage: ane-session-h13 "
				"[--dev N] [--lock FILE]\n");
			return 2;
		}
	}
	while ((len = getline(&line, &line_cap, stdin)) > 0) {
		char cmd[16], name[64], arg[4096];

		while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
			line[--len] = 0;
		}
		if (sscanf(line, "%15s", cmd) != 1) {
			continue;
		}
		if (!strcmp(cmd, "PING")) {
			replyf("OK PONG\n");
		} else if (!strcmp(cmd, "LOAD") &&
			   sscanf(line, "%*s %63s %4095s", name, arg) == 2) {
			if (cmd_load(&s, name, arg) < 0) {
				ret = 1;
				break;
			}
		} else if (!strcmp(cmd, "FREE") &&
			   sscanf(line, "%*s %63s", name) == 1) {
			if (cmd_free(&s, name) < 0) {
				ret = 1;
				break;
			}
		} else if (!strcmp(cmd, "CALL") &&
			   sscanf(line, "%*s %63s", name) == 1) {
			if (cmd_call(&s, name) < 0) {
				ret = 1;
				break;
			}
		} else if (!strcmp(cmd, "LOCK")) {
			if (cmd_lock(&s) < 0) {
				ret = 1;
				break;
			}
		} else if (!strcmp(cmd, "UNLOCK")) {
			if (cmd_unlock(&s) < 0) {
				ret = 1;
				break;
			}
		} else if (!strcmp(cmd, "QUIT")) {
			replyf("OK QUIT\n");
			break;
		} else {
			fprintf(stderr, "ane-session-h13: bad line: %s\n", line);
			ret = 2;
			break;
		}
	}
	free(line);
	session_free(&s);
	return s.failed ? 1 : ret;
}
