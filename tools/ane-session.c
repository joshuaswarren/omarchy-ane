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
 *   STATE <name> <in_idx> <out_idx> <run_bytes> <n_runs> + n_runs raw u64 (LE) offsets
 *                                 -> OK STATE <name> | ERR STATE <name> <reason>
 *       Makes input <in_idx> of <name> a state that stays in the session: after
 *       every CALL the <run_bytes> bytes at each offset of output <out_idx> are
 *       copied into the state (zero everywhere else), and the next CALL sends
 *       the state as that input. Both ports are then left out of the CALL
 *       payload and the reply. The state belongs to the NAME, never to the
 *       cached program, so names that share one cached program stay isolated.
 *       Indices are positions among the inputs and among the outputs.
 *   RESET [<name>]                -> OK RESET   (zero the states of one or all names)
 *   LOCK | UNLOCK                 -> OK LOCK | OK UNLOCK   (flock the lock file; no flock when
 *       ANE_LOCK_HELD_FD names an inherited fd that is open on that same file: the idle guard
 *       already holds the lock on it, and a second flock through our own fd would wait on it
 *       forever)
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
#include <limits.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
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

/* One resident state pair of a name: the logical bytes of output out_idx
 * (the runs) become input in_idx of the next CALL, zero elsewhere. */
struct session_state {
	uint32_t in_idx;
	uint32_t out_idx;
	uint64_t tile_bytes;
	uint64_t run_bytes;
	uint64_t nruns;
	uint64_t *offs;
	uint8_t *buf;		/* tile_bytes, zero outside the runs */
};

struct session_prog {
	char *name;
	struct session_key *key;	/* shared, owned by the key cache */
	uint64_t in_total;
	uint64_t out_total;
	struct session_state *st;	/* per NAME, never shared through the key */
	uint32_t nst;
};

/* One loaded program per DISTINCT (anec bytes, ports bytes) pair. A
 * LOAD whose key is already here returns the existing program with no
 * device work, so configure passes that FREE and re-LOAD the same
 * table cost nothing (measured 2026-10-09: each pass's duplicate
 * sections needed a fresh 223 MiB dma32 hole and the second pass had
 * none left). FREE drops the name binding only; the key dies at QUIT. */
struct session_key {
	char *key;
	struct ane_nn *nn;
	struct port_read pr;
	uint32_t n_in;
	uint32_t n_out;
	uint64_t demand;	/* page-aligned BO bytes this program holds */
};

struct session {
	struct session_prog progs[ANE_SESSION_MAX_PROGS];
	uint32_t nprogs;
	struct session_key keys[ANE_SESSION_MAX_PROGS];
	uint32_t nkeys;
	uint64_t demand;
	uint64_t bo_cap;  /* bytes; 0 = no cap known */
	uint64_t bo_used; /* driver-wide counter, 0 when unreadable */
	uint32_t max_progs;
	int dev;
	int failed;
	int lock_fd;
	int lock_held; /* 1 once ANE_LOCK_HELD_FD proved the caller holds the lock file */
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

/* FNV-1a 64 over a file's bytes: LOAD cache-key material. */
static int fnv_file(const char *path, uint64_t *out)
{
	void *buf;
	uint64_t size;
	uint64_t h = 0xcbf29ce484222325ull;
	uint8_t *b;
	uint64_t i;

	if (read_file_all(path, &buf, &size))
		return -1;
	b = buf;
	for (i = 0; i < size; i++)
		h = (h ^ b[i]) * 0x100000001b3ull;
	free(buf);
	*out = h;
	return 0;
}

static struct session_key *session_key_find(struct session *s,
					    const char *key)
{
	uint32_t i;

	for (i = 0; i < s->nkeys; i++) {
		if (!strcmp(s->keys[i].key, key))
			return &s->keys[i];
	}
	return NULL;
}

static void session_prog_free(struct session *s, struct session_prog *p)
{
	/* The key cache owns the program: FREE drops the name binding
	 * only, so a configure pass that FREEs and re-LOADs the same
	 * table does no device work at all. */
	uint32_t i;

	for (i = 0; i < p->nst; i++) {
		free(p->st[i].offs);
		free(p->st[i].buf);
	}
	free(p->st);
	free(p->name);
	s->nprogs--;
	memset(p, 0, sizeof(*p));
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
	struct session_key *k;
	struct port_read pr;
	struct ane_nn *nn;
	uint64_t ha = 0, hp = 0;
	uint64_t demand = 0;
	uint32_t i;
	char keybuf[48];

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
	if (fnv_file(anec, &ha) || fnv_file(ports_path, &hp)) {
		replyf("ERR LOAD %s unreadable\n", name);
		free_port_read(&pr);
		s->failed = 1;
		return -1;
	}
	snprintf(keybuf, sizeof(keybuf), "%llx:%llx",
		 (unsigned long long)ha, (unsigned long long)hp);
	k = session_key_find(s, keybuf);
	if (k) {
		/* Cache hit: a repeated (anec, ports) pair rebinds the
		 * name with no device work -- a configure pass that
		 * FREEs and re-LOADs the same table must not allocate
		 * anything (measured 2026-10-09: every pass's duplicate
		 * sections needed a fresh 223 MiB dma32 hole and the
		 * window had none left). The port table read above is a
		 * duplicate of the cached key's: release it, or every hit
		 * leaks one table. */
		free_port_read(&pr);
	} else {
		/* New key: the cache is bounded by the same knob as the
		 * name registry -- a session never needs more distinct
		 * programs than names. */
		if (s->nkeys >= s->max_progs) {
			replyf("ERR LOAD %s key-cache-full\n", name);
			free_port_read(&pr);
			s->failed = 1;
			return -1;
		}
		/* Guard 2: exact BO projection, still host-side (this
		 * build opens no device). */
		if (program_demand(anec, &pr, &demand)) {
			replyf("ERR LOAD %s build-failed\n", name);
			free_port_read(&pr);
			s->failed = 1;
			return -1;
		}
		if (s->bo_cap &&
		    s->demand + demand + s->bo_used > s->bo_cap) {
			replyf("ERR LOAD %s bo-cap: need %llu bytes, live "
			       "%llu plus driver %llu exceeds cap %llu "
			       "(refusing before the device)\n",
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
		k = &s->keys[s->nkeys++];
		k->key = strdup(keybuf);
		k->nn = nn;
		k->pr = pr;	/* the port table moves to the key */
		k->demand = demand;
		for (i = 0; i < pr.count; i++) {
			if (pr.ports[i].dir == 0)
				k->n_in++;
			else if (pr.ports[i].dir == 1)
				k->n_out++;
		}
		s->demand += demand;
	}
	/* Bind the name to the cached program. Guard 1 bounds nprogs, so
	 * a slot exists. */
	for (i = 0; i < ANE_SESSION_MAX_PROGS; i++) {
		if (!s->progs[i].name) {
			p = &s->progs[i];
			break;
		}
	}
	p->name = strdup(name);
	p->key = k;
	p->in_total = 0;
	p->out_total = 0;
	for (i = 0; i < k->pr.count; i++) {
		if (k->pr.ports[i].dir == 0)
			p->in_total += k->pr.ports[i].tile_bytes;
		else if (k->pr.ports[i].dir == 1)
			p->out_total += k->pr.ports[i].tile_bytes;
	}
	s->nprogs++;
	replyf("OK LOAD %s %u %u\n", name, (unsigned)k->n_in,
	       (unsigned)k->n_out);
	return 0;
}

/* Index into key->pr.ports of the idx-th port of a direction, or -1. */
static int port_at(const struct port_read *pr, uint32_t dir, uint32_t idx)
{
	uint32_t k, pos = 0;

	for (k = 0; k < pr->count; k++) {
		if (pr->ports[k].dir != dir) {
			continue;
		}
		if (pos == idx) {
			return (int)k;
		}
		pos++;
	}
	return -1;
}

static struct session_state *state_by_in(struct session_prog *p, uint32_t idx)
{
	uint32_t i;

	for (i = 0; i < p->nst; i++) {
		if (p->st[i].in_idx == idx) {
			return &p->st[i];
		}
	}
	return NULL;
}

static struct session_state *state_by_out(struct session_prog *p, uint32_t idx)
{
	uint32_t i;

	for (i = 0; i < p->nst; i++) {
		if (p->st[i].out_idx == idx) {
			return &p->st[i];
		}
	}
	return NULL;
}

#define ANE_SESSION_MAX_RUNS (1ull << 22)

/* A decimal u64 and nothing else: no sign, no space, no suffix, no overflow. */
static int parse_u64(const char *s, uint64_t *out)
{
	char *end = NULL;
	unsigned long long v;

	if (!s || !*s || *s < '0' || *s > '9') {
		return -1;
	}
	errno = 0;
	v = strtoull(s, &end, 10);
	if (errno || !end || *end) {
		return -1;
	}
	*out = (uint64_t)v;
	return 0;
}

static int cmd_state(struct session *s, const char *name, uint64_t in_idx,
		     uint64_t out_idx, uint64_t run_bytes, uint64_t nruns,
		     int fields_ok)
{
	struct session_prog *p = session_prog_find(s, name);
	struct session_state *st;
	const struct port_read *pr;
	uint64_t *offs = NULL;
	uint64_t i;
	int ki, ko;
	const char *why = NULL;

	/* The offsets follow the command: consume them first, so a refusal
	 * leaves the stream in step. A count that cannot be consumed safely
	 * is protocol death. */
	if (!nruns || nruns > ANE_SESSION_MAX_RUNS) {
		replyf("ERR STATE %s bad-run-count\n", name);
		s->failed = 1;
		return -1;
	}
	offs = malloc((size_t)nruns * sizeof(*offs));
	if (!offs || read_stream(offs, nruns * sizeof(*offs))) {
		free(offs);
		replyf("ERR STATE %s short-input\n", name);
		s->failed = 1;
		return -1;
	}
	if (!fields_ok || in_idx > UINT32_MAX || out_idx > UINT32_MAX) {
		why = "bad-field";
	} else if (s->failed) {
		why = "session-failed";
	} else if (!p) {
		why = "not-loaded";
	} else {
		pr = &p->key->pr;
		ki = port_at(pr, 0, (uint32_t)in_idx);
		ko = port_at(pr, 1, (uint32_t)out_idx);
		if (ki < 0 || ko < 0) {
			why = "no-such-port";
		} else if (pr->ports[ki].tile_bytes != pr->ports[ko].tile_bytes) {
			why = "tile-bytes-differ";
		} else if (state_by_in(p, (uint32_t)in_idx) ||
			   state_by_out(p, (uint32_t)out_idx)) {
			why = "already-resident";
		} else if (!run_bytes || run_bytes > pr->ports[ki].tile_bytes) {
			why = "bad-run-bytes";
		} else {
			for (i = 0; i < nruns; i++) {
				if (offs[i] > pr->ports[ki].tile_bytes ||
				    run_bytes > pr->ports[ki].tile_bytes - offs[i]) {
					why = "run-out-of-tile";
					break;
				}
			}
		}
	}
	if (why) {
		free(offs);
		replyf("ERR STATE %s %s\n", name, why);
		s->failed = 1;
		return -1;
	}
	st = realloc(p->st, (p->nst + 1) * sizeof(*st));
	if (!st) {
		free(offs);
		replyf("ERR STATE %s no-mem\n", name);
		s->failed = 1;
		return -1;
	}
	p->st = st;
	st = &p->st[p->nst];
	memset(st, 0, sizeof(*st));
	st->in_idx = (uint32_t)in_idx;
	st->out_idx = (uint32_t)out_idx;
	st->tile_bytes = pr->ports[ki].tile_bytes;
	st->run_bytes = run_bytes;
	st->nruns = nruns;
	st->offs = offs;
	st->buf = calloc(1, (size_t)st->tile_bytes);
	if (!st->buf) {
		free(offs);
		replyf("ERR STATE %s no-mem\n", name);
		s->failed = 1;
		return -1;
	}
	p->nst++;
	replyf("OK STATE %s\n", name);
	return 0;
}

static void state_zero(struct session_prog *p)
{
	uint32_t i;

	for (i = 0; i < p->nst; i++) {
		memset(p->st[i].buf, 0, (size_t)p->st[i].tile_bytes);
	}
}

static int cmd_reset(struct session *s, const char *name)
{
	uint32_t i;

	if (name) {
		struct session_prog *p = session_prog_find(s, name);

		if (!p) {
			replyf("ERR RESET %s not-loaded\n", name);
			s->failed = 1;
			return -1;
		}
		state_zero(p);
	} else {
		for (i = 0; i < ANE_SESSION_MAX_PROGS; i++) {
			if (s->progs[i].name) {
				state_zero(&s->progs[i]);
			}
		}
	}
	replyf("OK RESET\n");
	return 0;
}

static int cmd_call(struct session *s, const char *name)
{
	struct session_prog *p = session_prog_find(s, name);
	struct session_key *k;
	uint64_t off;
	uint32_t k2;
	struct timespec t0, t1;
	uint8_t *buf;
	uint64_t in_wire, out_wire;
	struct session_state *rs;
	uint8_t *scratch = NULL;

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
	k = p->key;
	/* Resident state inputs and outputs are not on the stream. */
	in_wire = p->in_total;
	out_wire = p->out_total;
	for (k2 = 0; k2 < p->nst; k2++) {
		in_wire -= p->st[k2].tile_bytes;
		out_wire -= p->st[k2].tile_bytes;
	}
	buf = malloc(in_wire ? (size_t)in_wire : 1);
	if (!buf) {
		replyf("ERR CALL %s no-mem\n", name);
		s->failed = 1;
		return -1;
	}
	if (read_stream(buf, in_wire)) {
		replyf("ERR CALL %s short-input\n", name);
		free(buf);
		s->failed = 1;
		return -1;
	}
	off = 0;
	for (k2 = 0; k2 < k->pr.count; k2++) {
		if (k->pr.ports[k2].dir != 0) {
			continue;
		}
		rs = state_by_in(p, port_dir_index(&k->pr, k2, 0));
		if (rs) {
			ane_m2_send(k->nn, rs->buf, rs->in_idx);
			continue;
		}
		ane_m2_send(k->nn, buf + off,
			    port_dir_index(&k->pr, k2, 0));
		off += k->pr.ports[k2].tile_bytes;
	}
	free(buf);
	/* Every buffer the reply needs is allocated before the device runs, so a
	 * failure answers ERR CALL with nothing half sent and no state touched. */
	buf = malloc(p->out_total ? (size_t)p->out_total : 1);
	if (p->nst) {
		uint64_t big = 0;

		for (k2 = 0; k2 < p->nst; k2++) {
			if (p->st[k2].tile_bytes > big) {
				big = p->st[k2].tile_bytes;
			}
		}
		scratch = malloc((size_t)big);
	}
	if (!buf || (p->nst && !scratch)) {
		free(buf);
		free(scratch);
		replyf("ERR CALL %s no-mem\n", name);
		s->failed = 1;
		return -1;
	}
	clock_gettime(CLOCK_MONOTONIC, &t0);
	if (ane_m2_exec(k->nn) < 0) {
		free(buf);
		free(scratch);
		replyf("ERR CALL %s exec-failed\n", name);
		s->failed = 1;
		return -1;
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	replyf("OK CALL %s %lld\n", name,
	       (long long)((t1.tv_sec - t0.tv_sec) * 1000000ll +
			   (t1.tv_nsec - t0.tv_nsec) / 1000));
	off = 0;
	for (k2 = 0; k2 < k->pr.count; k2++) {
		if (k->pr.ports[k2].dir != 1) {
			continue;
		}
		if (state_by_out(p, port_dir_index(&k->pr, k2, 1))) {
			continue;	/* carried after the reply, below */
		}
		ane_m2_read(k->nn, buf + off,
			    port_dir_index(&k->pr, k2, 1));
		off += k->pr.ports[k2].tile_bytes;
	}
	if (out_wire && fwrite(buf, 1, (size_t)out_wire, stdout) !=
				(size_t)out_wire) {
		free(buf);
		free(scratch);
		s->failed = 1;
		return -1;
	}
	fflush(stdout);
	free(buf);
	/* The carry runs after the reply payload is on the pipe, so the decoder
	 * is not kept waiting for it; the session is single threaded and reads
	 * the next command only after this returns, so the next CALL always
	 * sees the carried state. Only the logical runs enter the state: the
	 * device's bytes outside them never reach the next input, which is what
	 * the host-side pack (zeros plus the tensor) produces. */
	for (k2 = 0; k2 < k->pr.count; k2++) {
		uint64_t r;

		if (k->pr.ports[k2].dir != 1) {
			continue;
		}
		rs = state_by_out(p, port_dir_index(&k->pr, k2, 1));
		if (!rs) {
			continue;
		}
		ane_m2_read(k->nn, scratch, rs->out_idx);
		memset(rs->buf, 0, (size_t)rs->tile_bytes);
		for (r = 0; r < rs->nruns; r++) {
			memcpy(rs->buf + rs->offs[r], scratch + rs->offs[r],
			       (size_t)rs->run_bytes);
		}
	}
	free(scratch);
	return 0;
}

/* The idle guard runs a ticket with the ANE lock held on an inherited fd and names it in
 * ANE_LOCK_HELD_FD (plain decimal digits only). Trust it only when that fd is open on the very
 * file we would lock. Callers that run `flock /var/tmp/ane-run.lock` themselves (qwen_prog_run
 * ane_call, island_real, parakeet_encoder_islands) do not read it and still wait under a guard. */
static int caller_holds_lock(const char *lock_path)
{
	const char *env = getenv("ANE_LOCK_HELD_FD");
	char *end;
	long fd;
	struct stat held, lock;

	if (!env || *env < '0' || *env > '9') {
		return 0;
	}
	fd = strtol(env, &end, 10);
	if (*end || fd < 3 || fd > INT_MAX) {
		return 0;
	}
	return !fstat((int)fd, &held) && !stat(lock_path, &lock) && S_ISREG(held.st_mode) &&
	       held.st_dev == lock.st_dev && held.st_ino == lock.st_ino;
}

static int cmd_lock(struct session *s)
{
	if (s->lock_held) {
		replyf("OK LOCK\n");
		return 0;
	}
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
	for (i = 0; i < s->nkeys; i++) {
		ane_free(s->keys[i].nn);
		free_port_read(&s->keys[i].pr);
		free(s->keys[i].key);
	}
	if (s->lock_fd >= 0) {
		flock(s->lock_fd, LOCK_UN);
		close(s->lock_fd);
		s->lock_fd = -1;
	}
}

#ifdef ANE_SESSION_TEST
int ane_session_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
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
	s.lock_held = caller_holds_lock(s.lock_path);
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
		} else if (!strcmp(cmd, "STATE")) {
			char *name, *a, *b, *c, *d, *extra;
			char *save = NULL;
			uint64_t ia = 0, ib = 0, rb = 0, nr = 0;
			int ok;

			name = p1 ? strtok_r(p1, " ", &save) : NULL;
			a = name ? strtok_r(NULL, " ", &save) : NULL;
			b = a ? strtok_r(NULL, " ", &save) : NULL;
			c = b ? strtok_r(NULL, " ", &save) : NULL;
			d = c ? strtok_r(NULL, " ", &save) : NULL;
			extra = d ? strtok_r(NULL, " ", &save) : NULL;
			/* The run count sets the length of the offsets that follow: if
			 * it cannot be read exactly the stream cannot be resynchronised,
			 * so the session ends like any other malformed command. */
			if (!d || extra || parse_u64(d, &nr)) {
				fprintf(stderr, "ane-session: bad STATE\n");
				return 2;
			}
			ok = !parse_u64(a, &ia) && !parse_u64(b, &ib) &&
			     !parse_u64(c, &rb);
			cmd_state(&s, name, ia, ib, rb, nr, ok);
		} else if (!strcmp(cmd, "RESET")) {
			cmd_reset(&s, p1 && *p1 ? p1 : NULL);
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
