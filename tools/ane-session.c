/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Joshua Warren */

/* ane-session: one long-lived process holding ABI-2 (T6021) programs open,
 * so a decoder issues EXECs in-process instead of paying process spawn +
 * program load + BO alloc per call (the qwen_m2_decode --resident path).
 *
 * Protocol on stdin/stdout: text commands, raw surface bytes on the same
 * stream (no files). Sizes are the ports.json tile_bytes in ports.json
 * order; ane_m2_send/read index each direction by position in that same
 * order (ane_m2_program_build_ports io table: inputs, then outputs).
 *
 *   PING                          -> OK PONG
 *   LOAD <name> <anec> <ports.json>
 *                                 -> OK LOAD <name> <n_in> <n_out>
 *                                  | ERR LOAD <name> <reason>   (guard or device)
 *   FREE <name>                   -> OK FREE <name> | ERR FREE <name> <reason>
 *   CALL <name> + <in_total> raw bytes
 *                                 -> OK CALL <name> <exec_us> + <out_total> raw bytes
 *                                  | ERR CALL <name> <reason>
 *   LOCK | UNLOCK                 -> OK LOCK | OK UNLOCK   (flock the lock file)
 *   QUIT                          -> OK QUIT, exit 0 (1 if a CALL failed)
 *
 * Guards refuse a LOAD BEFORE any device open:
 *   - session program count against --max-progs (default 250, the driver's
 *     ANE_T6021_MAX_PROGRAMS; the driver dedups byte-identical LOADs across
 *     the boot, so this bounds this session's contribution),
 *   - the BO projection: page-aligned (size + guard) of every section and
 *     io BO of the live set, against --bo-cap-mb or
 *     /sys/module/ane_t6021/parameters/bo_total_{bytes,max_mb} when present.
 * A refused LOAD marks the session failed (exit 1) like a failed CALL.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

/* Reuse ane-run's strict port-table reader and io helpers verbatim;
 * its main() is renamed so the session can define its own. */
#define main ane_run_main
#include "ane-run.c"
#undef main

#define ANE_SESSION_MAX_PROGS 250
#define ANE_SESSION_SYSFS "/sys/module/ane_t6021/parameters"
/* Same default and env override as libane's ane_m2_guard(), so the
 * projection matches what bo_alloc will actually request. */
#define ANE_SESSION_GUARD_DEFAULT 0x4000ull

struct session_prog {
	char *name;
	struct ane_nn *nn;
	struct port_read pr;
	uint32_t n_in;
	uint32_t n_out;
	uint64_t in_total;
	uint64_t out_total;
	uint64_t demand; /* page-aligned BO bytes this program holds */
};

struct session {
	struct session_prog progs[ANE_SESSION_MAX_PROGS];
	uint32_t nprogs;
	uint64_t demand;
	uint64_t bo_cap;  /* bytes; 0 = no cap known */
	uint64_t bo_used; /* driver-wide counter, 0 when unreadable */
	uint32_t max_progs;
	int dev;
	int failed;
	int lock_fd;
	const char *lock_path;
};

/* Position of port k among the ports of its direction: the send/read
 * index and the io-table position of that direction. */
static uint32_t port_dir_index(const struct port_read *pr, uint32_t k,
			       uint32_t dir)
{
	uint32_t i, pos = 0;

	for (i = 0; i < k; i++) {
		pos += pr->ports[i].dir == dir;
	}
	return pos;
}

static uint64_t session_guard(void)
{
	const char *e = getenv("ANE_M2_GUARD");

	return e ? strtoull(e, NULL, 0) : ANE_SESSION_GUARD_DEFAULT;
}

static uint64_t page_align(uint64_t v)
{
	return (v + 0x3fffull) & ~0x3fffull;
}

static int sysfs_u64(const char *name, uint64_t *out)
{
	FILE *fp;
	unsigned long long v = 0;
	char path[256];

	snprintf(path, sizeof(path), ANE_SESSION_SYSFS "/%s", name);
	fp = fopen(path, "r");
	if (!fp) {
		return -1;
	}
	if (fscanf(fp, "%llu", &v) != 1) {
		fclose(fp);
		return -1;
	}
	fclose(fp);
	*out = (uint64_t)v;
	return 0;
}

static int read_file_all(const char *path, void **out, uint64_t *out_size)
{
	FILE *fp = fopen(path, "rb");
	long end;
	void *buf;

	if (!fp) {
		fprintf(stderr, "ane-session: failed to open %s\n", path);
		return -1;
	}
	if (fseek(fp, 0, SEEK_END) || (end = ftell(fp)) < 0 ||
	    fseek(fp, 0, SEEK_SET)) {
		fclose(fp);
		return -1;
	}
	buf = malloc((uint64_t)end);
	if (!buf) {
		fclose(fp);
		return -1;
	}
	if (fread(buf, 1, (size_t)end, fp) != (size_t)end) {
		fprintf(stderr, "ane-session: short read on %s\n", path);
		free(buf);
		fclose(fp);
		return -1;
	}
	fclose(fp);
	*out = buf;
	*out_size = (uint64_t)end;
	return 0;
}

/* Exact section + io BO demand of one program: six section BOs plus one
 * BO per io record, each page-aligned with the libane guard behind it,
 * exactly what bo_alloc charges to bo_total_bytes. Host-side: builds the
 * sections without a device and frees them again. */
static int program_demand(const char *anec, const struct port_read *pr,
			  uint64_t *out)
{
	void *buf;
	uint64_t size;
	struct ane_m2_model model;
	struct ane_m2_sections secs;
	uint64_t total = 0;
	uint32_t i;
	int err;

	if (read_file_all(anec, &buf, &size)) {
		return -1;
	}
	err = ane_m2_program_build_ports(buf, size, pr->ports, pr->count,
					 &model, &secs);
	free(buf);
	if (err) {
		fprintf(stderr, "ane-session: %s: ane_m2_program_build_ports "
				"failed (%d)\n",
			anec, err);
		return -1;
	}
	for (i = 0; i < ANE_M2_SEC_COUNT; i++) {
		total += page_align(secs.sec[i].size + session_guard());
	}
	for (i = 0; i < model.io_count; i++) {
		total += page_align(model.io[i].size + session_guard());
	}
	ane_m2_sections_free(&secs);
	*out = total;
	return 0;
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

static void session_prog_free(struct session *s, struct session_prog *p)
{
	char *name = p->name;

	if (p->nn) {
		ane_free(p->nn);
	}
	free_port_read(&p->pr);
	s->demand -= p->demand;
	s->nprogs--;
	memset(p, 0, sizeof(*p));
	free(name);
}

static void replyf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
}

/* Read exactly n bytes from stdin. 0 ok, -1 short (protocol death). */
static int read_stream(void *buf, uint64_t n)
{
	return n && fread(buf, 1, (size_t)n, stdin) != (size_t)n ? -1 : 0;
}

static int cmd_load(struct session *s, const char *name, const char *anec,
		    const char *ports_path)
{
	struct session_prog *p = NULL;
	struct port_read pr;
	struct ane_nn *nn;
	uint64_t demand = 0;
	uint64_t in_total = 0, out_total = 0;
	uint32_t i;

	if (session_prog_find(s, name)) {
		replyf("ERR LOAD %s already-loaded\n", name);
		s->failed = 1;
		return -1;
	}
	memset(&pr, 0, sizeof(pr));
	if (read_port_file(ports_path, &pr)) {
		replyf("ERR LOAD %s bad-ports\n", name);
		s->failed = 1;
		return -1;
	}
	/* Guard 1: session program count, before any work. */
	if (s->nprogs >= s->max_progs) {
		replyf("ERR LOAD %s max-progs %u reached (driver program "
		       "table bound; refusing before the device)\n",
		       name, (unsigned)s->max_progs);
		free_port_read(&pr);
		s->failed = 1;
		return -1;
	}
	/* Guard 2: exact BO projection, still host-side (this build opens
	 * no device). */
	if (program_demand(anec, &pr, &demand)) {
		replyf("ERR LOAD %s build-failed\n", name);
		free_port_read(&pr);
		s->failed = 1;
		return -1;
	}
	if (s->bo_cap && s->demand + demand + s->bo_used > s->bo_cap) {
		replyf("ERR LOAD %s bo-cap: need %llu bytes, live %llu plus "
		       "driver %llu exceeds cap %llu (refusing before the "
		       "device)\n",
		       name, (unsigned long long)demand,
		       (unsigned long long)s->demand,
		       (unsigned long long)s->bo_used,
		       (unsigned long long)s->bo_cap);
		free_port_read(&pr);
		s->failed = 1;
		return -1;
	}
	nn = ane_m2_init_ports(anec, pr.ports, pr.count, s->dev);
	if (!nn) {
		replyf("ERR LOAD %s device-open-failed\n", name);
		free_port_read(&pr);
		s->failed = 1;
		return -1;
	}
	for (i = 0; i < ANE_SESSION_MAX_PROGS; i++) {
		if (!s->progs[i].name) {
			p = &s->progs[i];
			break;
		}
	}
	if (!p) {
		/* Unreachable: guard 1 bounds nprogs <= max_progs <= 250. */
		ane_free(nn);
		free_port_read(&pr);
		replyf("ERR LOAD %s registry-full\n", name);
		s->failed = 1;
		return -1;
	}
	memset(p, 0, sizeof(*p));
	p->name = strdup(name);
	p->nn = nn;
	p->pr = pr;
	p->n_in = 0;
	p->n_out = 0;
	for (i = 0; i < pr.count; i++) {
		if (pr.ports[i].dir == 0) {
			p->n_in++;
			in_total += pr.ports[i].tile_bytes;
		} else if (pr.ports[i].dir == 1) {
			p->n_out++;
			out_total += pr.ports[i].tile_bytes;
		}
	}
	p->in_total = in_total;
	p->out_total = out_total;
	p->demand = demand;
	s->demand += demand;
	s->nprogs++;
	replyf("OK LOAD %s %u %u\n", name, (unsigned)p->n_in,
	       (unsigned)p->n_out);
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
		/* The payload length is unknown for an unknown program, so
		 * the stream is unrecoverable: fail the session. */
		replyf("ERR CALL %s not-loaded\n", name);
		s->failed = 1;
		return -1;
	}
	if (s->failed) {
		replyf("ERR CALL %s session-failed\n", name);
		return -1;
	}
	buf = malloc(p->in_total ? (size_t)p->in_total : 1);
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
	for (k = 0; k < p->pr.count; k++) {
		if (p->pr.ports[k].dir != 0) {
			continue;
		}
		ane_m2_send(p->nn, buf + off,
			    port_dir_index(&p->pr, k, 0));
		off += p->pr.ports[k].tile_bytes;
	}
	free(buf);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	if (ane_m2_exec(p->nn) < 0) {
		replyf("ERR CALL %s exec-failed\n", name);
		s->failed = 1;
		return -1;
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	replyf("OK CALL %s %lld\n", name,
	       (long long)((t1.tv_sec - t0.tv_sec) * 1000000ll +
			   (t1.tv_nsec - t0.tv_nsec) / 1000));
	buf = malloc(p->out_total ? (size_t)p->out_total : 1);
	if (!buf) {
		s->failed = 1;
		return -1;
	}
	off = 0;
	for (k = 0; k < p->pr.count; k++) {
		if (p->pr.ports[k].dir != 1) {
			continue;
		}
		ane_m2_read(p->nn, buf + off,
			    port_dir_index(&p->pr, k, 1));
		off += p->pr.ports[k].tile_bytes;
	}
	if (p->out_total && fwrite(buf, 1, (size_t)p->out_total, stdout) !=
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
			session_prog_free(s, &s->progs[i]);
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
	struct session s = { .lock_fd = -1, .dev = 0,
			     .max_progs = ANE_SESSION_MAX_PROGS };
	char *line = NULL;
	size_t line_cap = 0;
	ssize_t len;
	uint64_t v;
	int i, ret;
	const char *cap_mb = NULL;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--dev") && i + 1 < argc) {
			s.dev = (int)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--lock") && i + 1 < argc) {
			s.lock_path = argv[++i];
		} else if (!strcmp(argv[i], "--max-progs") && i + 1 < argc) {
			unsigned long n = strtoul(argv[++i], NULL, 0);

			s.max_progs = n > ANE_SESSION_MAX_PROGS
					      ? ANE_SESSION_MAX_PROGS
					      : (uint32_t)n;
		} else if (!strcmp(argv[i], "--bo-cap-mb") && i + 1 < argc) {
			cap_mb = argv[++i];
		} else {
			fprintf(stderr,
				"usage: ane-session [--dev N] [--lock PATH] "
				"[--max-progs N] [--bo-cap-mb N]\n");
			return 2;
		}
	}
	if (!s.lock_path) {
		s.lock_path = "/var/tmp/ane-run.lock";
	}
	if (cap_mb) {
		s.bo_cap = strtoull(cap_mb, NULL, 0) << 20;
	} else if (!sysfs_u64("bo_total_max_mb", &v)) {
		s.bo_cap = v << 20;
		if (sysfs_u64("bo_total_bytes", &s.bo_used)) {
			s.bo_used = 0;
		}
	} else {
		fprintf(stderr, "ane-session: no BO cap known; the bo-cap "
				"guard is off (pass --bo-cap-mb)\n");
	}

	while ((len = getline(&line, &line_cap, stdin)) > 0) {
		char *cmd = line, *p1;

		while (len > 0 &&
		       (line[len - 1] == '\n' || line[len - 1] == '\r')) {
			line[--len] = 0;
		}
		p1 = strchr(cmd, ' ');
		if (p1) {
			*p1++ = 0;
		}
		if (!strcmp(cmd, "PING")) {
			replyf("OK PONG\n");
		} else if (!strcmp(cmd, "QUIT")) {
			replyf("OK QUIT\n");
			break;
		} else if (!strcmp(cmd, "LOCK")) {
			cmd_lock(&s);
		} else if (!strcmp(cmd, "UNLOCK")) {
			cmd_unlock(&s);
		} else if (!strcmp(cmd, "LOAD")) {
			char *p2, *p3;

			if (!p1 || !(p2 = strchr(p1, ' ')) ||
			    !(p3 = strchr(p2 + 1, ' '))) {
				fprintf(stderr, "ane-session: bad LOAD\n");
				return 2;
			}
			*p2++ = 0;
			*p3++ = 0;
			cmd_load(&s, p1, p2, p3);
		} else if (!strcmp(cmd, "FREE")) {
			struct session_prog *p;

			if (!p1) {
				fprintf(stderr, "ane-session: bad FREE\n");
				return 2;
			}
			p = session_prog_find(&s, p1);
			if (!p) {
				replyf("ERR FREE %s not-loaded\n", p1);
				s.failed = 1;
				continue;
			}
			session_prog_free(&s, p);
			replyf("OK FREE %s\n", p1);
		} else if (!strcmp(cmd, "CALL")) {
			if (!p1) {
				fprintf(stderr, "ane-session: bad CALL\n");
				return 2;
			}
			cmd_call(&s, p1);
		} else {
			fprintf(stderr, "ane-session: unknown command %s\n",
				cmd);
			return 2;
		}
	}
	free(line);
	ret = s.failed ? 1 : 0;
	session_free(&s);
	return ret;
}
