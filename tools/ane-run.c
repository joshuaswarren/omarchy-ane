// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ane.h"
#include "ane_m2.h"

/* Populate the nchw shadow for one channel. Used by --check OP in
 * ane-run.c to attach the proven oracle layout to an island ANEC
 * whose encoder wrote zero in the header's nchw fields. */
void ane_set_oracle_nchw(struct ane_nn *nn, uint32_t ch,
			 const uint64_t nchw[6]);
#include "ane_f16_add.h"

/*
// ane-run -- run one ANEC program through libane and save its outputs.
//
//   ane-run --anec program.anec --in 0=a.fp16 --in 1=b.fp16 \
//           --out 0=y.fp16 [--repeat N] [--check OP]
//
// Input files must hold at least the channel's allocation bytes
// (ane_src_size). Output files are written with exactly ane_dst_size
// bytes. --check OP compares the output against the reference semantics
// of the fixture op (lab tools/h14_oracle.py).
//
// PROVEN on hardware (boot 8f468602): only the two-input add, fp16 with
// ties rounded away from zero. ASSUMED, not yet run on a device:
//   - mul, add-scalar, mul-scalar, real-div-scalar round half away from
//     zero like the add (mul products of two fp16 are exact before the
//     rounding; mul-scalar scales by 0.5 and real-div-scalar by 2.0, so
//     those two never round at all);
//   - relu, clip-low, clip-high are exact lane compares;
//   - matvec accumulates each output and rounds once; the device
//     accumulation width and order are unknown, so --check matvec
//     accepts a 2 ulp band and prints the max ulp difference.
// The valid lanes are the surface positions of the ANEC layout:
// elementwise ops pack one fp16 per 64-byte plane (nchw [1,512,1,1];
// valid half index % 32 == 0), matvec packs 256 dense halves (half index
// < 256). Every padding lane must be zero in the inputs and the output;
// both are checked.
*/

struct io_file {
	int set;
	uint32_t idx;
	const char *path;
};

/* Port-table path: --in NAME=FILE / --out NAME=FILE look up the port
 * by name and resolve the host-side idx from the sidecar order. The
 * sidecar carries inputs in MIL declaration order followed by the
 * single output; inputs are numbered 0..N-1 by their position among
 * the dir==0 entries. */
struct name_file {
	int set;
	const char *name;
	const char *path;
};

static int parse_io_arg(char *arg, struct io_file *io, const char *what)
{
	char *eq = strchr(arg, '=');
	if (!eq || eq == arg || !eq[1]) {
		fprintf(stderr, "bad --%s %s: want IDX=FILE\n", what, arg);
		return -1;
	}
	*eq = 0;
	io->set = 1;
	io->idx = (uint32_t)strtoul(arg, NULL, 0);
	io->path = eq + 1;
	return 0;
}

static int parse_name_arg(char *arg, struct name_file *nf,
			  const char *what)
{
	char *eq = strchr(arg, '=');
	if (!eq || eq == arg || !eq[1]) {
		fprintf(stderr, "bad --%s %s: want NAME=FILE\n", what, arg);
		return -1;
	}
	*eq = 0;
	nf->set = 1;
	nf->name = arg;
	nf->path = eq + 1;
	return 0;
}

/* Strict minimal JSON reader for the port-table sidecar. Accepts
 * exactly the flat schema: object with "program" (string), "ports"
 * (array of objects with "name", "shape" (4-int array), "buffer_id",
 * "channel", "bar_slot", "tile_bytes", "direction"). Everything else
 * is an error. Returned ports[] is in sidecar order; the caller
 * decides how to map it to the model. */
struct port_read {
	struct ane_m2_port_spec *ports;
	uint32_t count;
	uint32_t cap;
};

static int is_ws(int c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static const char *json_skip_ws(const char *p)
{
	while (*p && is_ws((unsigned char)*p)) {
		p++;
	}
	return p;
}

/* Parse a JSON string literal starting at p (which must point at the
 * opening quote). Returns the position right after the closing quote
 * in *end, and stores the unescaped contents into a heap-allocated
 * nul-terminated buffer returned via *out. Rejects control characters
 * and backslash escapes (the schema has none). */
static int json_read_string(const char *p, const char **end, char **out)
{
	const char *s = p;

	if (*s != '"') {
		return -1;
	}
	s++;
	while (*s && *s != '"') {
		if ((unsigned char)*s < 0x20 || *s == '\\') {
			return -1;
		}
		s++;
	}
	if (*s != '"') {
		return -1;
	}
	*out = strndup(p + 1, (size_t)(s - (p + 1)));
	if (!*out) {
		return -1;
	}
	*end = s + 1;
	return 0;
}

/* Parse a JSON integer literal (positive, optional hex via 0x, no
 * exponent). The schema has no floats, no negatives, no exponent. */
static int json_read_u64(const char *p, const char **end, uint64_t *out)
{
	const char *s = p;
	int seen = 0;
	uint64_t v = 0;
	int base = 10;

	if (*s == '0' && (s[1] == 'x' || s[1] == 'X')) {
		base = 16;
		s += 2;
	}
	while (*s) {
		int c = (unsigned char)*s;
		int d;

		if (c >= '0' && c <= '9') {
			d = c - '0';
		} else if (base == 16 && c >= 'a' && c <= 'f') {
			d = 10 + (c - 'a');
		} else if (base == 16 && c >= 'A' && c <= 'F') {
			d = 10 + (c - 'A');
		} else {
			break;
		}
		v = v * (uint64_t)base + (uint64_t)d;
		s++;
		seen = 1;
	}
	if (!seen) {
		return -1;
	}
	*out = v;
	*end = s;
	return 0;
}

/* Expect a single byte (with whitespace tolerance). */
static int json_expect(const char *p, const char **end, char c)
{
	p = json_skip_ws(p);
	if (*p != c) {
		return -1;
	}
	*end = p + 1;
	return 0;
}

/* Parse the shape [n, c, h, w] (4 uint64). Returns 0/-1. */
static int json_read_shape(const char *p, const char **end,
			   uint64_t shape[4])
{
	int i;

	p = json_skip_ws(p);
	if (json_expect(p, &p, '[')) {
		return -1;
	}
	for (i = 0; i < 4; i++) {
		p = json_skip_ws(p);
		if (json_read_u64(p, &p, &shape[i])) {
			return -1;
		}
		p = json_skip_ws(p);
		if (i + 1 < 4) {
			if (json_expect(p, &p, ',')) {
				return -1;
			}
		}
	}
	if (json_expect(p, &p, ']')) {
		return -1;
	}
	*end = p;
	return 0;
}

/* Add a port to a struct port_read, growing if needed. */
static int port_read_push(struct port_read *pr,
			  const struct ane_m2_port_spec *p)
{
	if (pr->count == pr->cap) {
		uint32_t nc = pr->cap ? pr->cap * 2 : 8;
		struct ane_m2_port_spec *np = realloc(pr->ports,
						     nc * sizeof(*np));
		if (!np) {
			return -1;
		}
		pr->ports = np;
		pr->cap = nc;
	}
	pr->ports[pr->count++] = *p;
	return 0;
}

/* Parse one port object {...}. The strict schema has exactly the keys
 * the C side reads (others are accepted but ignored); we still reject
 * unknown keys to keep the sidecar self-describing. */
static int json_read_port(const char *p, const char **end,
			  struct port_read *pr)
{
	const char *kstart, *kend;
	char *key = NULL;
	char *name_buf = NULL;
	struct ane_m2_port_spec ps = { 0 };
	int seen_dir = 0, seen_buf = 0, seen_slot = 0, seen_tile = 0;

	p = json_skip_ws(p);
	if (json_expect(p, &p, '{')) {
		return -1;
	}
	while (1) {
		p = json_skip_ws(p);
		if (*p == '}') {
			break;
		}
		kstart = p;
		if (json_read_string(kstart, &kend, &key)) {
			free(name_buf);
			return -1;
		}
		p = json_skip_ws(kend);
		if (json_expect(p, &p, ':')) {
			free(key);
			free(name_buf);
			return -1;
		}
		p = json_skip_ws(p);
		if (!strcmp(key, "name")) {
			free(name_buf);
			name_buf = NULL;
			if (json_read_string(p, &p, &name_buf)) {
				free(key);
				return -1;
			}
		} else if (!strcmp(key, "direction")) {
			char *d = NULL;
			if (json_read_string(p, &p, &d)) {
				free(key);
				free(name_buf);
				return -1;
			}
			if (!strcmp(d, "input")) {
				ps.dir = 0;
			} else if (!strcmp(d, "output")) {
				ps.dir = 1;
			} else {
				free(d);
				free(key);
				free(name_buf);
				return -1;
			}
			free(d);
			seen_dir = 1;
		} else if (!strcmp(key, "buffer_id") ||
			   !strcmp(key, "channel")) {
			uint64_t v;
			if (json_read_u64(p, &p, &v)) {
				free(key);
				free(name_buf);
				return -1;
			}
			ps.buffer_id = (uint32_t)v;
			seen_buf = 1;
		} else if (!strcmp(key, "bar_slot")) {
			uint64_t v;
			if (json_read_u64(p, &p, &v)) {
				free(key);
				free(name_buf);
				return -1;
			}
			ps.bar_slot = (uint32_t)v;
			seen_slot = 1;
		} else if (!strcmp(key, "tile_bytes")) {
			uint64_t v;
			if (json_read_u64(p, &p, &v)) {
				free(key);
				free(name_buf);
				return -1;
			}
			ps.tile_bytes = v;
			seen_tile = 1;
		} else if (!strcmp(key, "shape")) {
			uint64_t shp[4];
			if (json_read_shape(p, &p, shp)) {
				free(key);
				free(name_buf);
				return -1;
			}
		} else if (!strcmp(key, "dtype") ||
			   !strcmp(key, "strides") ||
			   !strcmp(key, "surface_bytes") ||
			   !strcmp(key, "confidence")) {
			/* Accepted but not consulted by the C reader; the
			 * Python coverage pass and the human verifier use
			 * these. Strictly typed so a typo like "tileByte"
			 * still fails loud. */
			const char *q;
			int depth = 0;
			int opened = 0;
			int in_str = 0;

			p = json_skip_ws(p);
			for (q = p; *q; q++) {
				int c = (unsigned char)*q;
				if (in_str) {
					if (c == '"' && q[-1] != '\\') {
						in_str = 0;
					}
					continue;
				}
				if (c == '"') {
					in_str = 1;
					continue;
				}
				if (c == '{' || c == '[') {
					depth++;
					opened = 1;
					continue;
				}
				if (c == '}' || c == ']') {
					/* A close before we've matched a
					 * nested structure is the parent
					 * object's close: stop without
					 * consuming (the outer loop will
					 * see the `}` and break). */
					if (!opened) {
						break;
					}
					depth--;
					if (depth == 0) {
						q++;
						break;
					}
					continue;
				}
				if (c == ',' && depth == 0) {
					break;
				}
			}
			if (depth != 0) {
				free(key);
				free(name_buf);
				return -1;
			}
			p = q;
		} else {
			free(key);
			free(name_buf);
			return -1;
		}
		free(key);
		key = NULL;
		p = json_skip_ws(p);
		if (*p == ',') {
			p++;
			continue;
		}
		if (*p == '}') {
			break;
		}
		free(name_buf);
		return -1;
	}
	if (json_expect(p, &p, '}')) {
		free(name_buf);
		return -1;
	}
	if (!seen_dir || !seen_buf || !seen_slot || !seen_tile || !name_buf) {
		free(name_buf);
		return -1;
	}
	ps.name = name_buf;
	if (port_read_push(pr, &ps)) {
		free(name_buf);
		return -1;
	}
	*end = p;
	return 0;
}

/* Top-level entry: {"program": ..., "ports": [ ... ]} plus tolerated
 * "anec"/"hwx"/"dma_coverage"/"ambiguities" siblings. We parse "program"
 * to confirm the schema header and "ports" to fill the array; the rest
 * is accepted and ignored for forward compatibility with later
 * MultiPort sidecars. */
static int json_read_top(const char *p, const char **end,
			 struct port_read *pr)
{
	char *prog = NULL;
	int seen_prog = 0, seen_ports = 0;

	p = json_skip_ws(p);
	if (json_expect(p, &p, '{')) {
		return -1;
	}
	while (1) {
		char *key = NULL;
		const char *kend;

		p = json_skip_ws(p);
		if (*p == '}') {
			break;
		}
		if (json_read_string(p, &kend, &key)) {
			return -1;
		}
		p = json_skip_ws(kend);
		if (json_expect(p, &p, ':')) {
			free(key);
			return -1;
		}
		p = json_skip_ws(p);
		if (!strcmp(key, "program")) {
			free(prog);
			if (json_read_string(p, &p, &prog)) {
				free(key);
				return -1;
			}
			seen_prog = 1;
		} else if (!strcmp(key, "ports")) {
			p = json_skip_ws(p);
			if (json_expect(p, &p, '[')) {
				free(key);
				return -1;
			}
			while (1) {
				p = json_skip_ws(p);
				if (*p == ']') {
					break;
				}
				if (json_read_port(p, &p, pr)) {
					free(key);
					return -1;
				}
				p = json_skip_ws(p);
				if (*p == ',') {
					p++;
					continue;
				}
				if (*p == ']') {
					break;
				}
				free(key);
				return -1;
			}
			if (json_expect(p, &p, ']')) {
				free(key);
				return -1;
			}
			seen_ports = 1;
		} else {
			/* Accept and skip a value of any shape:
			 * strings, numbers, arrays, objects. We do a
			 * bracket-balanced skip rather than a full parse
			 * so sidecar additions do not break us. */
			int depth = 0;
			int opened = 0;
			const char *q;
			int in_str = 0;

			p = json_skip_ws(p);
			for (q = p; *q; q++) {
				int c = (unsigned char)*q;
				if (in_str) {
					if (c == '"' && q[-1] != '\\') {
						in_str = 0;
					}
					continue;
				}
				if (c == '"') {
					in_str = 1;
					continue;
				}
				if (c == '{' || c == '[') {
					depth++;
					opened = 1;
					continue;
				}
				if (c == '}' || c == ']') {
					depth--;
					if (depth == 0) {
						if (opened) {
							q++;
						}
						break;
					}
					continue;
				}
				if (c == ',' && depth == 0) {
					break;
				}
			}
			if (depth != 0) {
				free(key);
				return -1;
			}
			p = q;
		}
		free(key);
		p = json_skip_ws(p);
		if (*p == ',') {
			p++;
			continue;
		}
		if (*p == '}') {
			break;
		}
		return -1;
	}
	if (json_expect(p, &p, '}')) {
		free(prog);
		return -1;
	}
	free(prog);
	if (!seen_prog || !seen_ports) {
		return -1;
	}
	*end = p;
	return 0;
}

static int read_port_file(const char *path, struct port_read *pr)
{
	FILE *fp = fopen(path, "rb");
	long end;
	char *buf;
	int err;

	if (!fp) {
		fprintf(stderr, "failed to open port table %s\n", path);
		return -1;
	}
	if (fseek(fp, 0, SEEK_END)) {
		fclose(fp);
		return -1;
	}
	end = ftell(fp);
	if (end < 0 || fseek(fp, 0, SEEK_SET)) {
		fclose(fp);
		return -1;
	}
	buf = malloc((size_t)end + 1);
	if (!buf) {
		fclose(fp);
		return -1;
	}
	if (fread(buf, 1, (size_t)end, fp) != (size_t)end) {
		fprintf(stderr, "short read on %s\n", path);
		free(buf);
		fclose(fp);
		return -1;
	}
	fclose(fp);
	buf[end] = 0;
	err = json_read_top(buf, (const char **)&end, pr);
	free(buf);
	if (err) {
		fprintf(stderr, "%s: malformed port-table JSON (strict "
				"reader accepts only the documented schema)\n",
			path);
	}
	return err;
}

/* Find a port by name. Returns the index in `pr` or -1. */
static int port_find(const struct port_read *pr, const char *name)
{
	uint32_t k;

	for (k = 0; k < pr->count; k++) {
		if (!strcmp(pr->ports[k].name, name)) {
			return (int)k;
		}
	}
	return -1;
}

/* Resolve the host-side idx (input position-among-inputs, or output 0)
 * for a named port. Returns the idx, or -1 if the name does not
 * exist or the direction does not match `dir`. */
static int port_host_idx(const struct port_read *pr, const char *name,
			 uint32_t dir)
{
	int idx = port_find(pr, name);
	uint32_t k;
	uint32_t pos = 0;

	if (idx < 0) {
		return -1;
	}
	if (dir == 1) {
		/* Single output: ane_m2_read indexes outputs by position. */
		return pr->ports[idx].dir == 1 ? 0 : -1;
	}
	for (k = 0; k < pr->count; k++) {
		if (pr->ports[k].dir == 0) {
			if (k == (uint32_t)idx) {
				return (int)pos;
			}
			pos++;
		}
	}
	return -1;
}

/* Print the operation-record pairs and io records that
 * ane_m2_program_build_ports would submit, without opening the device.
 * Format chosen for grep/ey: one line per pair, comma-separated, then
 * a header line, then the io records. */
static int dry_run_ports(const char *anec_path, const struct port_read *pr)
{
	void *buf = NULL;
	uint64_t size = 0;
	struct ane_m2_model model;
	struct ane_m2_sections secs;
	uint32_t i;
	uint32_t pair_count;
	FILE *fp;

	fp = fopen(anec_path, "rb");
	if (!fp) {
		fprintf(stderr, "failed to open %s\n", anec_path);
		return 1;
	}
	if (fseek(fp, 0, SEEK_END)) {
		fclose(fp);
		return 1;
	}
	size = (uint64_t)ftell(fp);
	if (fseek(fp, 0, SEEK_SET)) {
		fclose(fp);
		return 1;
	}
	buf = malloc(size);
	if (!buf) {
		fclose(fp);
		return 1;
	}
	if (fread(buf, 1, size, fp) != size) {
		fprintf(stderr, "short read on %s\n", anec_path);
		free(buf);
		fclose(fp);
		return 1;
	}
	fclose(fp);

	if (ane_m2_program_build_ports(buf, size, pr->ports, pr->count,
				       &model, &secs) < 0) {
		fprintf(stderr, "ane_m2_program_build_ports failed\n");
		free(buf);
		return 1;
	}
	ane_m2_sections_free(&secs);
	free(buf);

	pair_count = model.call_ref_count[0];
	printf("opref %u:", pair_count);
	for (i = 0; i < pair_count; i++) {
		printf("%s%u:%u", i ? "," : "",
		       model.call_refs[0][i].slot,
		       model.call_refs[0][i].tag);
	}
	printf("\n");
	printf("io %u:", model.io_count);
	for (i = 0; i < model.io_count; i++) {
		printf("%s%u:%llu", i ? "," : "",
		       model.io[i].buffer_id,
		       (unsigned long long)model.io[i].size);
	}
	printf("\n");
	return 0;
}

static void *read_exact(const char *path, uint64_t size)
{
	FILE *fp = fopen(path, "rb");
	void *buf;

	if (!fp) {
		fprintf(stderr, "failed to open %s\n", path);
		return NULL;
	}
	buf = malloc(size);
	if (!buf) {
		fclose(fp);
		return NULL;
	}
	if (fread(buf, 1, size, fp) != size) {
		fprintf(stderr, "%s: need at least %llu bytes\n", path,
			(unsigned long long)size);
		free(buf);
		fclose(fp);
		return NULL;
	}
	fclose(fp);
	return buf;
}

static int write_exact(const char *path, const void *buf, uint64_t size)
{
	FILE *fp = fopen(path, "wb");
	if (!fp) {
		fprintf(stderr, "failed to open %s for writing\n", path);
		return -1;
	}
	if (fwrite(buf, 1, size, fp) != size) {
		fprintf(stderr, "short write on %s\n", path);
		fclose(fp);
		return -1;
	}
	fclose(fp);
	return 0;
}

/* The checked fixture ops and their input arity.
 *
 * select: 3 inputs (a, b, cond). The (a, b, cond) ordering at --in 0,1,2
 * matches the ANEC channel order 5,6,7 = MIL declaration order for
 * select(a = a_in, b = b_in, cond = c_in). The runtime channel
 * placement is proven for the parakeet island-b-select-runtime
 * fixture (oracle gasel_rrb_1x8x375x375): ch5 = a (fp16), ch6 = b
 * (fp16), ch7 = cond (bool), ch4 = y (fp16).
 *
 * bmm: 2 inputs (x, w). The MIL declaration order is (x, w); the
 * encoder swaps them: ch5 = w (second MIL input), ch6 = x (first
 * MIL input), ch4 = product. Reference formula:
 *   out[b,c,m,n] = sum_k x[b,c,m,k] * w[b,c,k,n].
 * PROVEN on the M2 (jw14m2-linux boot 2a4f18f7) for the three encoded
 * islands (island-c-pv, island-a-kt, island-a-attn-p1); the swap is
 * consistent with all the H14 encoder templates
 * (H14IslandTemplates.inc).
 *
 * rms: 1 input (x). Gamma is loaded from --weights (fp16 [C], row-major;
 * matches the MIL BLOBFILE gamma offset 64 in the encoder oracle).
 *
 * nchw-population: the ANEC header nchw[] fields are zero (the encoder
 * does not populate them). bmm_shape_nchw() / select_shape_nchw() /
 * rms_shape_nchw() fill them from the per-island oracle tables below
 * keyed by the tiles[] signature, so the check tool sees the proven
 * surface layouts. */
enum {
	CHK_ADD, CHK_MUL, CHK_RELU, CHK_ADD_SCALAR, CHK_MUL_SCALAR,
	CHK_REAL_DIV, CHK_CLIP_LOW, CHK_CLIP_HIGH, CHK_MATVEC,
	CHK_SELECT, CHK_BMM, CHK_RMS, CHK_COUNT
};

static const struct {
	const char *name;
	int ins;
	int matvec;
} check_ops[CHK_COUNT] = {
	{ "add", 2, 0 },
	{ "mul", 2, 0 },
	{ "relu", 1, 0 },
	{ "add-scalar", 1, 0 },
	{ "mul-scalar", 1, 0 },
	{ "real-div-scalar", 1, 0 },
	{ "clip-low", 1, 0 },
	{ "clip-high", 1, 0 },
	{ "matvec", 1, 1 },
	{ "select", 3, 0 },
	{ "bmm", 2, 0 },
	{ "rms", 1, 0 },
};

static int parse_check_op(const char *name)
{
	int i;

	for (i = 0; i < CHK_COUNT; i++) {
		if (!strcmp(name, check_ops[i].name)) {
			return i;
		}
	}
	return -1;
}

/* ulp size of a double value at fp16 granularity (the matvec band). */
static double f16_ulp(double v)
{
	double a = fabs(v);
	int e;

	if (a == 0.0) {
		return 0x1p-24; /* subnormal quantum */
	}
	frexp(a, &e);
	if (e < -13) {
		return 0x1p-24;
	}
	return ldexp(1.0, e - 11);
}

/* Derive the matvec geometry (M, K, N) from the loaded ANEC header.
 * Returns 0 on a recognized matvec and writes the dimensions; -1 on
 * any disagreement (the channel tile count is denominated in 0x4000
 * bytes, the matvec surface has K fp16 inputs on channel 5 and N fp16
 * outputs on channel 4, and M is the model row count, here taken as
 * 1 for the proven shapes -- the compiler only emits single-row
 * matvec programs for M=1; the M=8 forms run N output channels with
 * 8 row buffers packed into the surface and are detected via the
 * 8-row mark). */
static int matvec_shape(const struct ane_nn *nn, uint32_t *M, uint32_t *K,
			uint32_t *N)
{
	const struct anec *a = to_anec(nn);

	if (ane_src_count(nn) != 1 || ane_dst_count(nn) != 1) {
		fprintf(stderr, "--check matvec needs 1 input + 1 output\n");
		return -1;
	}
	/* nchw = (n, c, h, w, plane bytes, row bytes): h is M, w is the
	 * element count of a row. The ANEC surface allocation is padded to
	 * whole 0x4000 tiles, so the tile counts say nothing about K, N. */
	if (a->nchw[4][0] != 1 || a->nchw[4][1] != 1 ||
	    a->nchw[5][0] != 1 || a->nchw[5][1] != 1 ||
	    a->nchw[4][2] != a->nchw[5][2]) {
		fprintf(stderr, "--check matvec: unexpected io shape\n");
		return -1;
	}
	*M = (uint32_t)a->nchw[4][2];
	*N = (uint32_t)a->nchw[4][3];
	*K = (uint32_t)a->nchw[5][3];
	if (!*M || !*N || !*K || a->nchw[4][5] != (uint64_t)*N * 2 ||
	    a->nchw[5][5] != (uint64_t)*K * 2) {
		fprintf(stderr, "--check matvec: io rows are not dense\n");
		return -1;
	}
	return 0;
}

/* Apple's matvec weight packing permutation is enforced by the compiler
 * when it produces kernel.bin; --check matvec only needs the raw [N, K]
 * weight table to compute the oracle. The constant section was already
 * validated by the host self-check (tools/ane-selfcheck --check) before
 * any device run. */

/* Weight table for --check matvec, set from --weights. */
static const char *weights_path;

/* Select: read the nchw to derive dtype, row stride, and valid-lane count.
 *
 * The cond operand (channel 7 in the Parakeet select-8head island; the
 * 3rd declared input of MIL select(a, b, cond)) is a bool tensor whose
 * surface is byte-packed and row-aligned to 64 bytes (row_bytes =
 * align_up(W, 64) when each element is one byte; the fp16 surfaces use
 * row_bytes = align_up(W*2, 64) and a half index % (W*2/2) advances
 * one element). The decoder's dtype inference is INFERRED from the
 * MIL declaration, not a decoded fw site.
 *
 * Returns 0 on a recognized select shape and writes per-channel shapes;
 * -1 on disagreement (e.g. fp16 channel 7 row stride inconsistent with
 * bool packing).
 */
static int select_shape(const struct ane_nn *nn,
			uint64_t *elements,
			uint64_t *cond_row_bytes,
			uint64_t *a_row_bytes,
			int *cond_is_bool)
{
	const struct anec *a = to_anec(nn);
	uint64_t N, C, H, W;
	uint64_t fp16_row, bool_row;

	if (ane_src_count(nn) != 3 || ane_dst_count(nn) != 1) {
		fprintf(stderr, "--check select needs 3 inputs + 1 output\n");
		return -1;
	}
	/* channels 5 (a), 6 (b) are fp16; channel 7 (cond) is bool (or
	 * the encoder-decided shape per the MIL). We require N, C, H, W
	 * to agree across the three input channels and the output. */
	if (a->nchw[4][0] != a->nchw[5][0] ||
	    a->nchw[4][1] != a->nchw[5][1] ||
	    a->nchw[4][2] != a->nchw[5][2] ||
	    a->nchw[4][3] != a->nchw[5][3] ||
	    a->nchw[4][0] != a->nchw[6][0] ||
	    a->nchw[4][1] != a->nchw[6][1] ||
	    a->nchw[4][2] != a->nchw[6][2] ||
	    a->nchw[4][3] != a->nchw[6][3] ||
	    a->nchw[4][0] != a->nchw[7][0] ||
	    a->nchw[4][1] != a->nchw[7][1] ||
	    a->nchw[4][2] != a->nchw[7][2] ||
	    a->nchw[4][3] != a->nchw[7][3]) {
		fprintf(stderr, "--check select: input shapes disagree\n");
		return -1;
	}
	N = a->nchw[4][0];
	C = a->nchw[5][1];
	H = a->nchw[4][2];
	W = a->nchw[4][3];
	if (!N || !C || !H || !W) {
		fprintf(stderr, "--check select: zero-shape channel\n");
		return -1;
	}
	/* cond is bool (1 byte per element) iff row_bytes == align_up(W, 64)
	 * AND a->nchw[7][4] == H * align_up(W, 64). For fp16 cond (not
	 * exercised by the Parakeet islands), row_bytes would be
	 * align_up(W*2, 64) and the rule would be inverted. */
	fp16_row = (W * 2 + 63) & ~63ULL;
	bool_row = (W + 63) & ~63ULL;
	if (a->nchw[7][5] == bool_row &&
	    a->nchw[7][4] == H * bool_row) {
		*cond_is_bool = 1;
		*cond_row_bytes = bool_row;
	} else if (a->nchw[7][5] == fp16_row &&
		   a->nchw[7][4] == H * fp16_row) {
		*cond_is_bool = 0;
		*cond_row_bytes = fp16_row;
	} else {
		fprintf(stderr, "--check select: cond row stride %llu does "
			"not match either bool (%llu) or fp16 (%llu) packing\n",
			(unsigned long long)a->nchw[7][5],
			(unsigned long long)bool_row,
			(unsigned long long)fp16_row);
		return -1;
	}
	if (a->nchw[5][5] != fp16_row || a->nchw[6][5] != fp16_row ||
	    a->nchw[4][5] != fp16_row) {
		fprintf(stderr, "--check select: fp16 channels row stride "
			"is not align_up(W*2, 64) (a=%llu b=%llu y=%llu, want "
			"%llu)\n",
			(unsigned long long)a->nchw[5][5],
			(unsigned long long)a->nchw[6][5],
			(unsigned long long)a->nchw[4][5],
			(unsigned long long)fp16_row);
		return -1;
	}
	*a_row_bytes = fp16_row;
	*elements = N * C * H * W;
	return 0;
}

/* Batched matmul (bmm): derive M, K, N from the loaded ANEC header.
 *
 * Shapes (parakeet-island oracles under
 * /home/joshuawarren/src/mil-hwx-h14-mint-wt/research/oracles/h14/,
 * recorded by the H14 encoder at H14IslandTemplates.inc):
 *   ch 5 (--in 0) = w, the SECOND MIL input (matmul w = k): [B, C, K, N]
 *                  in NCHW ([B, C, K, N] -> rows are K, cols are N).
 *   ch 6 (--in 1) = x, the FIRST MIL input (matmul x = q): [B, C, M, K]
 *                  in NCHW ([B, C, M, K] -> rows are M, cols are K).
 *   ch 4 (--out 0)= product: [B, C, M, N].
 *
 * PROVEN on the M2 (jw14m2-linux boot 2a4f18f7) for three of the
 * three encoded islands: the kernel sees w on ch 5, x on ch 6, out on
 * ch 4. The MIL declaration order is (x, w) but the encoder swaps
 * the operands into ch 5/6; this swap is the proven pattern.
 *
 * ANEC header nchw[] fields are zero (the encoder does not populate
 * them); bmm_shape_nchw() below rewrites them from the ANEC tiles[]
 * and the known per-island oracle shapes. fp32 accumulate in fp64,
 * 2 ulp band; condition-normalized error.
 */
static int bmm_shape(struct ane_nn *nn, uint32_t *Bm, uint32_t *Cm,
		     uint32_t *Mm, uint32_t *Km, uint32_t *Nm);

/* bmm nchw oracle table (H14IslandTemplates.inc). Each row says
 * "if tiles[4]=t4, tiles[5]=t5, tiles[6]=t6, then the per-channel
 * nchw is fixed and the matmul formula is:
 *   out[b,c,m,n] = sum_k x[b,c,m,k] * w[b,c,k,n]
 * where x is on ch 6, w on ch 5, out on ch 4 (the MIL operand swap). */
struct bmm_oracle {
	uint32_t t4, t5, t6; /* tile counts at channels 4, 5, 6 */
	uint64_t N, C, M, K, N_dim; /* [B,C,M,K,N] dims */
	const char *name;
};

static const struct bmm_oracle bmm_oracles[] = {
	/* island-c-pv = gabmm_r3_m375_k375_n128_tx0_ty0_b8 */
	{ 47, 47, 141, 1, 8, 375, 375, 128, "island-c-pv" },
	/* island-a-kt = gabmm_r3_m375_k128_n749_tx0_ty0_b8 */
	{ 282, 96, 47, 1, 8, 375, 128, 749, "island-a-kt" },
	/* island-a-attn-p1 = gabmm_r4heads_m375_k128_n375_tx0_ty0_b8 */
	{ 141, 48, 47, 1, 8, 375, 128, 375, "island-a-attn-p1" },
};

static int bmm_populate_nchw(struct ane_nn *nn)
{
	const struct anec *a = to_anec(nn);
	const struct bmm_oracle *o = NULL;
	size_t i;
	uint32_t t4 = a->tiles[4], t5 = a->tiles[5], t6 = a->tiles[6];
	uint64_t w_n[6], x_n[6], y_n[6];

	for (i = 0; i < sizeof(bmm_oracles) / sizeof(bmm_oracles[0]); i++) {
		if (bmm_oracles[i].t4 == t4 && bmm_oracles[i].t5 == t5 &&
		    bmm_oracles[i].t6 == t6) {
			o = &bmm_oracles[i];
			break;
		}
	}
	if (!o) {
		fprintf(stderr, "--check bmm: no oracle for tiles "
			"[4]=%u [5]=%u [6]=%u (not a known Parakeet island)\n",
			t4, t5, t6);
		return -1;
	}
	/* The ANEC struct in libane/ane.h declared nchw[ch] const; the
	 * header bytes are byte-identical to the file on disk, but the
	 * encoder does not populate them so they are zero in every
	 * island. The host-side check tool needs the proven surface
	 * layout to compute a reference; libane exposes a mutable
	 * nchw shadow through the ane_set_oracle_nchw() helper. */
	w_n[0] = o->N; w_n[1] = o->C; w_n[2] = o->K; w_n[3] = o->N_dim;
	w_n[4] = 0;
	w_n[5] = (o->N_dim * 2 + 63) & ~63ULL;
	x_n[0] = o->N; x_n[1] = o->C; x_n[2] = o->M; x_n[3] = o->K;
	x_n[4] = 0;
	x_n[5] = (o->K * 2 + 63) & ~63ULL;
	y_n[0] = o->N; y_n[1] = o->C; y_n[2] = o->M; y_n[3] = o->N_dim;
	y_n[4] = 0;
	y_n[5] = (o->N_dim * 2 + 63) & ~63ULL;
	ane_set_oracle_nchw(nn, 5, w_n);
	ane_set_oracle_nchw(nn, 6, x_n);
	ane_set_oracle_nchw(nn, 4, y_n);
	(void)nn;
	(void)a;
	(void)o;
	return 0;
}

static int bmm_shape(struct ane_nn *nn, uint32_t *Bm, uint32_t *Cm,
		     uint32_t *Mm, uint32_t *Km, uint32_t *Nm)
{
	const struct anec *a = to_anec(nn);
	uint64_t out_W, out_row, w_row, x_row;

	if (ane_src_count(nn) != 2 || ane_dst_count(nn) != 1) {
		fprintf(stderr, "--check bmm needs 2 inputs + 1 output\n");
		return -1;
	}
	if (a->nchw[4][0] == 0 && a->nchw[5][0] == 0 && a->nchw[6][0] == 0) {
		if (bmm_populate_nchw(nn) < 0) {
			return -1;
		}
	}
	/* Output must be a [B, C, M, N] dense row of fp16; the row stride
	 * is align_up(N*2, 64), matching a single N-valued output row. */
	out_W = a->nchw[4][3];
	out_row = (out_W * 2 + 63) & ~63ULL;
	if (a->nchw[4][5] != out_row) {
		fprintf(stderr, "--check bmm: output row stride %llu does "
			"not match align_up(N*2, 64)=%llu\n",
			(unsigned long long)a->nchw[4][5],
			(unsigned long long)out_row);
		return -1;
	}
	/* w (ch 5) and x (ch 6) share B, C with the output. The matmul
	 * reduction axis K is w's H axis (= x's W axis). */
	if (a->nchw[5][0] != a->nchw[4][0] ||
	    a->nchw[5][1] != a->nchw[4][1] ||
	    a->nchw[6][0] != a->nchw[4][0] ||
	    a->nchw[6][1] != a->nchw[4][1]) {
		fprintf(stderr, "--check bmm: B,C disagree across channels\n");
		return -1;
	}
	if (a->nchw[5][2] != a->nchw[6][3]) {
		fprintf(stderr, "--check bmm: w.K (%llu) != x.K (%llu)\n",
			(unsigned long long)a->nchw[5][2],
			(unsigned long long)a->nchw[6][3]);
		return -1;
	}
	/* Row strides are align_up(W*2, 64) for the fp16 inner dim. */
	w_row = (a->nchw[5][3] * 2 + 63) & ~63ULL;
	x_row = (a->nchw[6][3] * 2 + 63) & ~63ULL;
	if (a->nchw[5][5] != w_row || a->nchw[6][5] != x_row) {
		fprintf(stderr, "--check bmm: input row strides disagree\n");
		return -1;
	}
	*Bm = (uint32_t)a->nchw[4][0];
	*Cm = (uint32_t)a->nchw[4][1];
	*Mm = (uint32_t)a->nchw[4][2];
	*Km = (uint32_t)a->nchw[5][2];
	*Nm = (uint32_t)a->nchw[4][3];
	if (!*Bm || !*Cm || !*Mm || !*Km || !*Nm) {
		fprintf(stderr, "--check bmm: zero-shape channel\n");
		return -1;
	}
	return 0;
}

/* select_check: out = cond ? a : b on the valid lanes.
 *
 * UNPROVEN: the (a, b, cond) ordering at --in 0,1,2 follows the MIL
 * declaration order (channels 5,6,7), but the runtime input list is
 * bound by channel id alone — there is no fw decoder site that ties
 * a specific input slot to a or cond.
 */
static int select_check(struct ane_nn *nn, struct io_file *in,
			struct io_file *out)
{
	uint64_t elements = 0;
	uint64_t cond_row_bytes = 0;
	uint64_t a_row_bytes = 0;
	int cond_is_bool = 0;
	uint64_t a_size, b_size, c_size, y_size;
	uint16_t *a = NULL, *b = NULL, *y = NULL;
	uint8_t *c = NULL;
	uint64_t pad_in = 0, pad_out = 0, exact = 0;
	uint64_t N, C, H, W, pad_per_row_a, pad_per_row_c;
	uint64_t n, ch, h, w, p, a_row_off, c_row_off, y_row_off, off;
	uint64_t a_off, c_off;
	uint16_t av, bv, yv, want;
	uint16_t cv;
	int ok;

	if (!in[0].set || in[0].idx != 0 || !in[1].set || in[1].idx != 1 ||
	    !in[2].set || in[2].idx != 2 || !out[0].set || out[0].idx != 0) {
		fprintf(stderr, "--check select needs --in 0, --in 1, --in 2, "
			"--out 0 (UNPROVEN channel ordering)\n");
		return -1;
	}
	if (select_shape(nn, &elements, &cond_row_bytes, &a_row_bytes,
			 &cond_is_bool) < 0) {
		return -1;
	}
	a_size = ane_src_size(nn, 0);
	b_size = ane_src_size(nn, 1);
	c_size = ane_src_size(nn, 2);
	y_size = ane_dst_size(nn, 0);
	a = read_exact(in[0].path, a_size);
	b = read_exact(in[1].path, b_size);
	c = read_exact(in[2].path, c_size);
	y = read_exact(out[0].path, y_size);
	if (!a || !b || !c || !y) {
		free(a); free(b); free(c); free(y);
		return -1;
	}
	N = to_anec(nn)->nchw[4][0];
	C = to_anec(nn)->nchw[4][1];
	H = to_anec(nn)->nchw[4][2];
	W = to_anec(nn)->nchw[4][3];
	pad_per_row_a = (a_row_bytes - W * 2) / 2;
	pad_per_row_c = cond_is_bool ? (cond_row_bytes - W) :
				      (cond_row_bytes - W * 2) / 2;
	for (n = 0; n < N; n++) {
		for (ch = 0; ch < C; ch++) {
			for (h = 0; h < H; h++) {
				a_row_off = ((n * C + ch) * H + h) *
					    a_row_bytes;
				c_row_off = ((n * C + ch) * H + h) *
					    cond_row_bytes;
				for (w = 0; w < W; w++) {
					a_off = a_row_off + w * 2;
					c_off = c_row_off + w * (cond_is_bool ? 1 : 2);
					av = a[a_off / 2];
					bv = b[a_off / 2];
					if (cond_is_bool) {
						cv = c[c_off];
					} else {
						memcpy(&cv, c + c_off, sizeof(cv));
						cv &= 0x7fff;
					}
					yv = y[a_off / 2];
					want = cv ? av : bv;
					exact += want == yv;
				}
				for (p = 0; p < pad_per_row_a; p++) {
					off = a_row_off + W * 2 + p * 2;
					pad_in += (a[off / 2] != 0 ||
						   b[off / 2] != 0);
				}
				for (p = 0; p < pad_per_row_c; p++) {
					off = c_row_off + (W + p) * (cond_is_bool ? 1 : 2);
					if (cond_is_bool) {
						pad_in += c[off] != 0;
					} else {
						memcpy(&cv, c + off, sizeof(cv));
						pad_in += cv != 0;
					}
				}
			}
		}
	}
	for (n = 0; n < N; n++) {
		for (ch = 0; ch < C; ch++) {
			for (h = 0; h < H; h++) {
				y_row_off = ((n * C + ch) * H + h) *
					    a_row_bytes;
				for (p = 0; p < pad_per_row_a; p++) {
					off = y_row_off + W * 2 + p * 2;
					pad_out += y[off / 2] != 0;
				}
			}
		}
	}
	ok = !pad_in && !pad_out && exact == elements;
	printf("select [%llu,%llu,%llu,%llu] cond %s: %llu/%llu lanes "
	       "bit-exact (UNPROVEN (a,b,cond) ordering), padding %s: %s\n",
	       (unsigned long long)N, (unsigned long long)C,
	       (unsigned long long)H, (unsigned long long)W,
	       cond_is_bool ? "bool (1B/elem)" : "fp16 (2B/elem)",
	       (unsigned long long)exact, (unsigned long long)elements,
	       pad_in || pad_out ? "lanes NONZERO" : "lanes zero",
	       ok ? "PASS" : "FAIL");
	free(a); free(b); free(c); free(y);
	return ok ? 0 : -1;
}

/* bmm_check: batched matmul reference (fp32 accumulate in fp64,
 * condition-normalized error like the matvec check). */
static int bmm_check(struct ane_nn *nn, struct io_file *in,
		     struct io_file *out)
{
	uint32_t B = 0, C_ = 0, M = 0, K = 0, N = 0;
	uint64_t a_size, b_size, y_size;
	uint16_t *w = NULL, *x = NULL, *y = NULL;
	uint64_t lanes = 0;
	uint64_t in_band = 0, exact = 0;
	uint64_t near_zero = 0;
	uint64_t max_ulp_milli = 0;
	double max_nerr = 0.0;
	uint64_t pad_out = 0;
	uint64_t w_row, x_row, o_row;
	uint64_t x_bc_off, w_bc_off, o_bc_off;
	uint64_t x_row_off, o_row_off;
	uint32_t bc, m, n, k;
	double ax, ay, acc, sumabs, err, nerr, diff;
	uint16_t got, want;
	uint64_t milli;
	int ok;

	if (!in[0].set || in[0].idx != 0 || !in[1].set || in[1].idx != 1 ||
	    !out[0].set || out[0].idx != 0) {
		fprintf(stderr, "--check bmm needs --in 0, --in 1, --out 0 "
			"(slot0 = ch5 = w [B,C,K,N]; slot1 = ch6 = x [B,C,M,K])\n");
		return -1;
	}
	if (bmm_shape(nn, &B, &C_, &M, &K, &N) < 0) {
		return -1;
	}
	a_size = ane_src_size(nn, 0);
	b_size = ane_src_size(nn, 1);
	y_size = ane_dst_size(nn, 0);
	/* Device binding (libane/ane_m2.c:545-553, proven on M2 by IslandDebug
	 * 2026-09-30): user io 0 -> ANEC channel 5 = w [B,C,K,N] in compiler
	 * templates (H14IslandTemplates.inc Tensors4/6/9); user io 1 ->
	 * channel 6 = x [B,C,M,K]; output -> channel 4. Reference:
	 * out[b,c,m,n] = sum_k x[b,c,m,k] * w[b,c,k,n] with x on slot1
	 * (M rows of align(K*2,64)) and w on slot0 (K rows of align(N*2,64)). */
	w = read_exact(in[0].path, a_size);
	x = read_exact(in[1].path, b_size);
	y = read_exact(out[0].path, y_size);
	if (!w || !x || !y) {
		free(w); free(x); free(y);
		return -1;
	}
	w_row = (N * 2 + 63) & ~63ULL; /* K rows of N */
	x_row = (K * 2 + 63) & ~63ULL; /* M rows of K */
	o_row = (N * 2 + 63) & ~63ULL;
	lanes = (uint64_t)B * C_ * M * N;
	for (bc = 0; bc < B * C_; bc++) {
		w_bc_off = (uint64_t)bc * K * w_row;
		x_bc_off = (uint64_t)bc * M * x_row;
		o_bc_off = (uint64_t)bc * M * o_row;
		for (m = 0; m < M; m++) {
			x_row_off = x_bc_off + (uint64_t)m * x_row;
			o_row_off = o_bc_off + (uint64_t)m * o_row;
			for (n = 0; n < N; n++) {
				acc = 0.0;
				sumabs = 0.0;
				for (k = 0; k < K; k++) {
					ax = ane_f16_to_f64(
						x[(x_row_off + k * 2) / 2]);
					ay = ane_f16_to_f64(
					w[(w_bc_off + (uint64_t)k * w_row + (uint64_t)n * 2) / 2]);
					acc += ax * ay;
					sumabs += fabs(ax * ay);
				}
				got = y[(o_row_off + n * 2) / 2];
				err = fabs(ane_f16_to_f64(got) - acc);
				nerr = sumabs > 0.0 ?
					err / (sumabs * 0x1p-11) : 0.0;
				if (nerr > max_nerr) {
					max_nerr = nerr;
				}
				want = ane_f16_round_half_away(acc);
				{
					double ref64 = ane_f16_to_f64(want);
					double u = f16_ulp(ref64);
					double cond_tol = sumabs * 0x1p-9; /* 4 cond-units */
					/* 3 ulps of the reference covers the
					 * fp16 rounding of the result plus
					 * one extra ULP from accumulation-
					 * order difference between the
					 * device's fp32 sum and the fp64
					 * reference. The 4 cond-units band
					 * is the standard sum-of-products
					 * relative tolerance. */
					double u_tol = 3.0 * u;
					double tol = (u_tol > cond_tol) ?
						      u_tol : cond_tol;
					double absdiff = fabs(
						ane_f16_to_f64(got) - ref64);

					diff = absdiff / u;
					milli = (uint64_t)(diff * 1000.0 + 0.5);
					if (sumabs > 0x1p-10) {
						in_band += absdiff <= tol;
					} else {
						in_band += (got == want);
						near_zero++;
					}
				}
				exact += want == got;
				if (milli > max_ulp_milli) {
					max_ulp_milli = milli;
				}
			}
		}
	}
	/* Output padding: half indices N..o_row/2-1 per row must be zero. */
	for (bc = 0; bc < B * C_; bc++) {
		o_bc_off = (uint64_t)bc * M * o_row;
		for (m = 0; m < M; m++) {
			o_row_off = o_bc_off + (uint64_t)m * o_row;
			for (k = N; k < o_row / 2; k++) {
				pad_out += y[o_row_off / 2 + k] != 0;
			}
		}
	}
	ok = !pad_out && in_band == lanes;
	printf("bmm B=%u C=%u M=%u K=%u N=%u: %llu/%llu lanes within 2 ulp "
	       "or 4 cond-units (%llu bit-exact, %llu near-zero sumabs "
	       "cancellation), max %llu.%03llu ulp, max %.3f cond-units; "
	       "padding out %s: %s\n",
	       B, C_, M, K, N,
	       (unsigned long long)in_band, (unsigned long long)lanes,
	       (unsigned long long)exact,
	       (unsigned long long)near_zero,
	       (unsigned long long)(max_ulp_milli / 1000),
	       (unsigned long long)(max_ulp_milli % 1000), max_nerr,
	       pad_out ? "lanes NONZERO" : "lanes zero",
	       ok ? "PASS" : "FAIL");
	free(w); free(x); free(y);
	return ok ? 0 : -1;
}

/* Forward declarations for the helpers that follow rms_check. */
static int rms_shape(const struct ane_nn *nn, uint32_t *Cm);

/* rms_check: y = x * gamma / sqrt(E[x^2] + eps*max^2) with eps = 2^-17. */
static int rms_check(struct ane_nn *nn, struct io_file *in,
		     struct io_file *out)
{
	uint32_t C = 0;
	uint64_t a_size, y_size;
	uint16_t *a = NULL, *y = NULL, *gamma = NULL;
	const uint32_t channel_stride = 32;
	uint64_t in_band = 0, exact = 0;
	uint64_t max_ulp_milli = 0;
	double max_nerr = 0.0;
	double max_abs = 0.0;
	double sum_sq = 0.0;
	double mean_sq, eps, rscaled;
	double xv, gv, want_v, err, sumabs, nerr, diff;
	uint16_t want, got;
	uint64_t milli;
	uint32_t i, lanes;
	int ok;

	if (!in[0].set || in[0].idx != 0 || !out[0].set || out[0].idx != 0) {
		fprintf(stderr, "--check rms needs --in 0, --out 0\n");
		return -1;
	}
	if (rms_shape(nn, &C) < 0) {
		return -1;
	}
	if (!weights_path) {
		fprintf(stderr, "--check rms needs --weights FILE (the MIL "
			"BLOBFILE; gamma is fp16 [C] starting at offset 64 "
			"per the encoder oracle)\n");
		return -1;
	}
	a_size = ane_src_size(nn, 0);
	y_size = ane_dst_size(nn, 0);
	if (a_size < (uint64_t)C * channel_stride * 2 ||
	    y_size < (uint64_t)C * channel_stride * 2) {
		fprintf(stderr, "--check rms: io surface smaller than C=%u "
			"rows of %u elements; the stride-%u accessor would "
			"over-read\n", C, channel_stride, channel_stride);
		return -1;
	}
	a = read_exact(in[0].path, a_size);
	y = read_exact(out[0].path, y_size);
	/* The MIL BLOBFILE has a 64-byte sub-header before gamma; skip it
	 * (per the encoder oracle for rms_norm_decomposed). */
	gamma = read_exact(weights_path, 64 + (uint64_t)C * 2);
	if (!a || !y || !gamma) {
		free(a); free(y); free(gamma);
		return -1;
	}
	lanes = C;
	/* Find the max(|x|) and E[x^2] over all C channels (single batch,
	 * single head, single H, single W). */
	for (i = 0; i < C; i++) {
		xv = ane_f16_to_f64(a[i * channel_stride]);
		if (fabs(xv) > max_abs) {
			max_abs = fabs(xv);
		}
		sum_sq += xv * xv;
	}
	mean_sq = sum_sq / (double)C;
	eps = 0x1p-17;
	rscaled = sqrt(mean_sq + eps * max_abs * max_abs);
	/* Tolerance: 2 ulp band (UNPROVEN — the device's order of the
	 * max-abs, mean, sqrt, and 1/rscaled is not decoded from fw). */
	for (i = 0; i < C; i++) {
		xv = ane_f16_to_f64(a[i * channel_stride]);
		gv = ane_f16_to_f64(gamma[64 / 2 + i]);
		want_v = xv * gv / rscaled;
		want = ane_f16_round_half_away(want_v);
		got = y[i * channel_stride];
		err = fabs(ane_f16_to_f64(got) - want_v);
		sumabs = fabs(xv) + fabs(gv) + fabs(rscaled);
		nerr = sumabs > 0.0 ? err / (sumabs * 0x1p-11) : 0.0;
		if (nerr > max_nerr) {
			max_nerr = nerr;
		}
		diff = fabs(ane_f16_to_f64(got) -
			    ane_f16_to_f64(want)) /
		       f16_ulp(ane_f16_to_f64(want));
		milli = (uint64_t)(diff * 1000.0 + 0.5);
		in_band += diff <= 2.0 || nerr <= 4.0;
		exact += want == got;
		if (milli > max_ulp_milli) {
			max_ulp_milli = milli;
		}
	}
	ok = in_band == lanes;
	printf("rms C=%u: %llu/%llu lanes within 2 ulp or 4 cond-units "
	       "(%llu bit-exact), max %llu.%03llu ulp, max %.3f cond-units; "
	       "tolerance UNPROVEN: %s\n",
	       C,
	       (unsigned long long)in_band, (unsigned long long)lanes,
	       (unsigned long long)exact,
	       (unsigned long long)(max_ulp_milli / 1000),
	       (unsigned long long)(max_ulp_milli % 1000), max_nerr,
	       ok ? "PASS" : "FAIL");
	free(a); free(y); free(gamma);
	return ok ? 0 : -1;
}

/* RMS: derive C from the loaded ANEC header. The MIL
 * rms_norm_decomposed chain computes
 *   scaled = x / max; sq = scaled^2; mean = E[sq]; eps = 2^-17
 *   rscaled = sqrt(mean + eps) * max
 *   y = (x / rscaled) * gamma
 * which is equivalent to y = x * gamma / sqrt(E[x^2] + eps*max^2).
 *
 * The gamma tensor is read from --weights (fp16 [C] row-major, the
 * BLOBFILE layout from the encoder oracle; offset 64 in weights.bin).
 * Tolerance: 2 ulp band like matvec; cond-normalized in fp16 ulps.
 * UNPROVEN: the device's accumulation order, the rounding of
 * 1/rscaled, and the fp16 epsilon constant are not decoded from fw.
 */
static int rms_shape(const struct ane_nn *nn, uint32_t *Cm)
{
	const struct anec *a = to_anec(nn);

	if (ane_src_count(nn) != 1 || ane_dst_count(nn) != 1) {
		fprintf(stderr, "--check rms needs 1 input + 1 output\n");
		return -1;
	}
	if (a->nchw[4][0] != a->nchw[5][0] ||
	    a->nchw[4][1] != a->nchw[5][1] ||
	    a->nchw[4][2] != a->nchw[5][2] ||
	    a->nchw[4][3] != a->nchw[5][3]) {
		fprintf(stderr, "--check rms: input/output shapes disagree\n");
		return -1;
	}
	if (a->nchw[5][2] != 1 || a->nchw[5][3] != 1) {
		fprintf(stderr, "--check rms: H,W must be 1 (channel-only); "
			"got H=%llu W=%llu\n",
			(unsigned long long)a->nchw[5][2],
			(unsigned long long)a->nchw[5][3]);
		return -1;
	}
	*Cm = (uint32_t)a->nchw[5][1];
	if (!*Cm) {
		fprintf(stderr, "--check rms: zero channel count\n");
		return -1;
	}
	return 0;
}

static int check_program(int op, struct ane_nn *nn, struct io_file *in,
			 struct io_file *out)
{
	int two_in;
	int matvec;
	uint64_t a_size, b_size = 0, y_size, n, lanes, i, j;
	uint64_t pad_in = 0, pad_out = 0, exact = 0, in_band = 0;
	uint64_t max_ulp_milli = 0;
	double max_nerr = 0.0;
	uint16_t *a, *b = NULL, *y;
	int ok;
	uint32_t M = 1, K = 0, N = 0;
	uint16_t *w = NULL;

	if (op == CHK_SELECT) {
		return select_check(nn, in, out);
	}
	if (op == CHK_BMM) {
		return bmm_check(nn, in, out);
	}
	if (op == CHK_RMS) {
		return rms_check(nn, in, out);
	}
	two_in = check_ops[op].ins == 2;
	matvec = check_ops[op].matvec;

	if (!in[0].set || in[0].idx != 0 || !out[0].set || out[0].idx != 0 ||
	    (two_in && (!in[1].set || in[1].idx != 1))) {
		fprintf(stderr, "--check %s needs --in 0%s, --out 0\n",
			check_ops[op].name, two_in ? ", --in 1" : "");
		return -1;
	}
	a_size = ane_src_size(nn, 0);
	y_size = ane_dst_size(nn, 0);
	if (two_in) {
		b_size = ane_src_size(nn, 1);
	}
	if (!a_size || !y_size || y_size != a_size ||
	    (two_in && b_size != a_size)) {
		fprintf(stderr, "--check %s: io channel sizes disagree\n",
			check_ops[op].name);
		return -1;
	}
	n = a_size / 2;
	lanes = matvec ? 256 : n / 32;

	a = read_exact(in[0].path, a_size);
	if (two_in) {
		b = read_exact(in[1].path, b_size);
	}
	y = read_exact(out[0].path, y_size);
	if (!a || !y || (two_in && !b)) {
		free(a);
		free(b);
		free(y);
		return -1;
	}

	/* Valid-lane predicate of the surface layout; padding must be zero
	 * in the inputs and the output. */
	if (matvec) {
		if (matvec_shape(nn, &M, &K, &N) < 0) {
			free(a);
			free(b);
			free(y);
			return -1;
		}
		if (!weights_path) {
			fprintf(stderr, "--check matvec needs --weights FILE\n");
			free(a);
			free(b);
			free(y);
			return -1;
		}
		w = (uint16_t *)read_exact(weights_path, (uint64_t)N * K * 2);
		if (!w) {
			free(a);
			free(b);
			free(y);
			return -1;
		}
		lanes = (uint64_t)M * N;
	}
	#define A_VALID(i) (matvec ? (i) < (uint64_t)M * K : ((i) % 32) == 0)
	#define Y_VALID(i) (matvec ? (i) < (uint64_t)M * N : ((i) % 32) == 0)
	for (i = 0; i < n; i++) {
		pad_in += !A_VALID(i) && ((a[i] != 0) || (b && b[i] != 0));
		pad_out += !Y_VALID(i) && y[i] != 0;
	}
	for (j = 0; j < lanes; j++) {
		double va;
		uint16_t want;

		i = matvec ? j : j * 32;
		va = matvec ? 0.0 : ane_f16_to_f64(a[i]);

		if (matvec) {
			double acc = 0.0, sumabs = 0.0, err, nerr;
			double diff;
			uint64_t milli;
			uint32_t k;

			uint64_t row = j / N, col = j % N;

			for (k = 0; k < K; k++) {
				double term = ane_f16_to_f64(a[row * K + k]) *
					      ane_f16_to_f64(w[col * K + k]);

				acc += term;
				sumabs += fabs(term);
			}
			/* Condition-normalized error: |device - exact| in units
			 * of 2^-11 * sum|a_k w_k|, the fp16 rounding error of
			 * one partial sum. Cancellation makes the ulp of the
			 * result a poor scale. */
			err = fabs(ane_f16_to_f64(y[i]) - acc);
			nerr = sumabs > 0.0 ? err / (sumabs * 0x1p-11) : 0.0;
			if (nerr > max_nerr) {
				max_nerr = nerr;
			}
			want = ane_f16_round_half_away(acc);
			/* Device accumulation order unknown: a 2 ulp band,
			 * max |got - want| printed in milli-ulp. */
			diff = fabs(ane_f16_to_f64(y[i]) -
				    ane_f16_to_f64(want)) /
			       f16_ulp(ane_f16_to_f64(want));
			milli = (uint64_t)(diff * 1000.0 + 0.5);

			in_band += diff <= 2.0 || nerr <= 4.0;
			if (milli > max_ulp_milli) {
				max_ulp_milli = milli;
			}
			exact += want == y[i];
			continue;
		}
		switch (op) {
		case CHK_ADD:
			want = ane_f16_round_half_away(va +
				ane_f16_to_f64(b[i]));
			break;
		case CHK_MUL:
			want = ane_f16_round_half_away(va *
				ane_f16_to_f64(b[i]));
			break;
		case CHK_RELU:
			want = va > 0.0 ? a[i] : (uint16_t)0x0000;
			break;
		case CHK_ADD_SCALAR:
			want = ane_f16_round_half_away(va + 0.5);
			break;
		case CHK_MUL_SCALAR:
			want = ane_f16_round_half_away(va * 0.5);
			break;
		case CHK_REAL_DIV:
			want = ane_f16_round_half_away(va * 2.0);
			break;
		case CHK_CLIP_LOW:
			want = va >= 0.5 ? a[i] : (uint16_t)0x3800;
			break;
		case CHK_CLIP_HIGH:
			want = va <= 0.5 ? a[i] : (uint16_t)0x3800;
			break;
		default:
			want = 0;
			break;
		}
		exact += want == y[i];
	}
	#undef A_VALID
	#undef Y_VALID

	ok = !pad_in && !pad_out &&
	     (matvec ? in_band == lanes : exact == lanes);
	if (matvec) {
		printf("matvec M=%u K=%u N=%u: %llu/%llu lanes within 2 ulp or "
		       "4 cond-units (%llu bit-exact), max %llu.%03llu ulp, "
		       "max %.3f cond-units; padding %s: %s\n",
		       M, K, N,
		       (unsigned long long)in_band, (unsigned long long)lanes,
		       (unsigned long long)exact,
		       (unsigned long long)(max_ulp_milli / 1000),
		       (unsigned long long)(max_ulp_milli % 1000), max_nerr,
		       pad_in || pad_out ? "lanes NONZERO" : "lanes zero",
		       ok ? "PASS" : "FAIL");
	} else {
		printf("%llu/%llu lanes bit-exact vs the %s reference, "
		       "padding %s: %s\n",
		       (unsigned long long)exact, (unsigned long long)lanes,
		       check_ops[op].name,
		       pad_in || pad_out ? "lanes NONZERO" : "lanes zero",
		       ok ? "PASS" : "FAIL");
	}
	free(a);
	free(b);
	free(y);
	free(w);
	return ok ? 0 : -1;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return (x > y) - (x < y);
}

static void usage(void)
{
	fprintf(stderr,
		"usage: ane-run --anec FILE [--in IDX=FILE]... "
		"[--out IDX=FILE]... [--repeat N] [--time] [--check OP] "
		"[--weights FILE]\n"
		"   or: ane-run --anec FILE --ports FILE.json [--in NAME=FILE]...\n"
		"                                  [--out NAME=FILE]... [--dry-run]\n"
		"OP: add mul relu add-scalar mul-scalar real-div-scalar "
		"clip-low clip-high matvec\n"
		"    select bmm rms\n"
		"  select: 3 inputs --in 0=a --in 1=b --in 2=cond\n"
		"    (device-proven order: --in 0 -> ch5 = the cond=0\n"
		"    branch, --in 1 -> ch6 = the cond=1 branch, --in 2 ->\n"
		"    ch7 = cond; cond is bool 1B/elem, row-aligned to\n"
		"    64 B). Output: out = cond ? a : b.\n"
		"  bmm: 2 inputs --in 0=x --in 1=y (UNPROVEN ordering).\n"
		"    Output: out[b,c,m,n] = sum_k x[b,c,m,k] * y[b,c,k,n],\n"
		"    fp64 accumulate, 2 ulp band.\n"
		"  rms: 1 input --in 0=x (rows of 64 B; row r holds x[r]).\n"
		"    Device-proven (IslandSelectRms2): gamma = 0.5 vector\n"
		"    baked at kernel.bin 0x1080; the engine materializes\n"
		"    fp16(0.5/rs) and computes y[r] = fp16(x[r]*fp16(0.5/rs))\n"
		"    for r in 64..2047, rs = sqrt(E[x^2 over 2048] +\n"
		"    2^-17 * max(|x|)^2). Measured band: 2 ulp of the result\n"
		"    (over 99.8 percent bit-exact on 3 seeds).\n"
		"--weights: matvec weight table (fp16 [N, K]); rms BLOBFILE.\n"
		"  Required with --check matvec and --check rms.\n"
		"--ports FILE.json: port table sidecar ({\"program\": \"...\",\n"
		"  \"ports\": [{\"name\":..., \"direction\":..., \"buffer_id\":...,\n"
		"  \"bar_slot\":..., \"tile_bytes\":..., \"shape\":[n,c,h,w]} ... ]}).\n"
		"  Switches to the named-port path; --in/--out take NAME=FILE.\n"
		"  Strict JSON: anything outside the documented keys is rejected.\n"
		"--dry-run: with --ports, print the operation-record pairs and\n"
		"  io records the runner would submit, then exit. No device.\n");
}

int main(int argc, char **argv)
{
	const char *anec = NULL;
	const char *ports_path = NULL;
	struct ane_nn *nn;
	struct io_file ins[8] = { 0 };
	struct io_file outs[8] = { 0 };
	struct name_file nins[8] = { 0 };
	struct name_file nouts[8] = { 0 };
	uint32_t repeat = 1;
	int timing = 0;
	int check = -1;
	int dry_run = 0;
	int ports_mode = 0;
	int ret;
	int k;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--anec") && i + 1 < argc) {
			anec = argv[++i];
		} else if (!strcmp(argv[i], "--in") && i + 1 < argc) {
			if (ports_mode) {
				int slot = -1;
				for (k = 0; k < 8; k++) {
					if (!nins[k].set) {
						slot = k;
						break;
					}
				}
				if (slot < 0) {
					fprintf(stderr, "too many --in args "
						"(max 8)\n");
					return 2;
				}
				if (parse_name_arg(argv[++i], &nins[slot],
						   "in")) {
					return 2;
				}
			} else {
				int slot = -1;
				for (k = 0; k < 8; k++) {
					if (!ins[k].set) {
						slot = k;
						break;
					}
				}
				if (slot < 0) {
					fprintf(stderr, "too many --in args "
						"(max 8)\n");
					return 2;
				}
				if (parse_io_arg(argv[++i], &ins[slot], "in"))
					return 2;
			}
		} else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
			if (ports_mode) {
				int slot = -1;
				for (k = 0; k < 8; k++) {
					if (!nouts[k].set) {
						slot = k;
						break;
					}
				}
				if (slot < 0) {
					fprintf(stderr, "too many --out args "
						"(max 8)\n");
					return 2;
				}
				if (parse_name_arg(argv[++i], &nouts[slot],
						   "out")) {
					return 2;
				}
			} else {
				int slot = -1;
				for (k = 0; k < 8; k++) {
					if (!outs[k].set) {
						slot = k;
						break;
					}
				}
				if (slot < 0) {
					fprintf(stderr, "too many --out args "
						"(max 8)\n");
					return 2;
				}
				if (parse_io_arg(argv[++i], &outs[slot], "out"))
					return 2;
			}
		} else if (!strcmp(argv[i], "--time")) {
			timing = 1;
		} else if (!strcmp(argv[i], "--repeat") && i + 1 < argc) {
			repeat = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--check") && i + 1 < argc) {
			check = parse_check_op(argv[++i]);
			if (check < 0) {
				usage();
				return 2;
			}
		} else if (!strcmp(argv[i], "--weights") && i + 1 < argc) {
			weights_path = argv[++i];
		} else if (!strcmp(argv[i], "--ports") && i + 1 < argc) {
			ports_path = argv[++i];
			ports_mode = 1;
		} else if (!strcmp(argv[i], "--dry-run")) {
			dry_run = 1;
		} else {
			usage();
			return 2;
		}
	}
	if (!anec) {
		usage();
		return 2;
	}
	if (!ports_mode) {
		/* Legacy path: unchanged. */
	} else if (dry_run) {
		/* Dry-run path: no device. */
		struct port_read pr = { 0 };

		if (read_port_file(ports_path, &pr) < 0) {
			free(pr.ports);
			return 1;
		}
		ret = dry_run_ports(anec, &pr);
		for (k = 0; k < (int)pr.count; k++) {
			free((void *)pr.ports[k].name);
		}
		free(pr.ports);
		return ret;
	}

	nn = ane_init(anec);

	if (!nn) {
		fprintf(stderr, "ane_init failed on %s\n", anec);
		return 1;
	}

	if (ports_mode) {
		/* Replace ins/outs with name-resolved indices. We need
		 * anec.src_count and anec.dst_count to be valid for the
		 * name-bound io entries; if a name-based --in asks for
		 * a port whose dir == 0 but nn->m2 reports fewer
		 * inputs than the sidecar promises, we surface the
		 * mismatch. The legacy ane_init path uses
		 * ane_m2_program_build which synthesises inputs from
		 * the anec input_count; for the port-table path we
		 * also need those inputs to be allocated. */
		struct port_read pr = { 0 };

		if (read_port_file(ports_path, &pr) < 0) {
			ane_free(nn);
			free(pr.ports);
			return 1;
		}
		for (k = 0; k < 8; k++) {
			if (nins[k].set) {
				int idx = port_host_idx(&pr, nins[k].name, 0);
				if (idx < 0) {
					fprintf(stderr, "no input port named "
						"%s\n", nins[k].name);
					ane_free(nn);
					free(pr.ports);
					return 1;
				}
				ins[k].set = 1;
				ins[k].idx = (uint32_t)idx;
				ins[k].path = nins[k].path;
			}
			if (nouts[k].set) {
				int idx = port_host_idx(&pr, nouts[k].name, 1);
				if (idx < 0) {
					fprintf(stderr, "no output port named "
						"%s\n", nouts[k].name);
					ane_free(nn);
					free(pr.ports);
					return 1;
				}
				outs[k].set = 1;
				outs[k].idx = (uint32_t)idx;
				outs[k].path = nouts[k].path;
			}
		}
		for (k = 0; k < (int)pr.count; k++) {
			free((void *)pr.ports[k].name);
		}
		free(pr.ports);
	}

	ret = 0;
	for (k = 0; k < 8 && !ret; k++) {
		uint64_t size;
		void *buf;

		if (!ins[k].set) {
			break;
		}
		size = __ane_src_size(nn, ins[k].idx);
		if (!size) {
			fprintf(stderr, "no input channel at index %u\n",
				ins[k].idx);
			ret = 1;
			break;
		}
		buf = read_exact(ins[k].path, size);
		if (!buf) {
			ret = 1;
			break;
		}
		__ane_send(nn, buf, ins[k].idx);
		free(buf);
	}

	{
		double *lat = timing ? calloc(repeat, sizeof(*lat)) : NULL;
		uint32_t done = 0;

		for (uint32_t r = 0; r < repeat && !ret; r++) {
			struct timespec t0, t1;

			clock_gettime(CLOCK_MONOTONIC, &t0);
			if (ane_exec(nn) < 0) {
				fprintf(stderr, "ane_exec failed\n");
				ret = 1;
			}
			clock_gettime(CLOCK_MONOTONIC, &t1);
			if (lat) {
				lat[done++] = (t1.tv_sec - t0.tv_sec) * 1e3 +
					      (t1.tv_nsec - t0.tv_nsec) / 1e6;
			}
		}
		if (lat && done) {
			qsort(lat, done, sizeof(*lat), cmp_double);
			printf("exec ms over %u calls: min %.3f p10 %.3f p25 %.3f "
			       "median %.3f p75 %.3f p90 %.3f p99 %.3f max %.3f\n",
			       done, lat[0], lat[done / 10], lat[done / 4],
			       lat[done / 2], lat[(done * 3) / 4],
			       lat[(done * 9) / 10], lat[(done * 99) / 100],
			       lat[done - 1]);
		}
		free(lat);
	}

	for (k = 0; k < 8 && !ret; k++) {
		uint64_t size;
		void *buf;

		if (!outs[k].set) {
			break;
		}
		size = __ane_dst_size(nn, outs[k].idx);
		if (!size) {
			fprintf(stderr, "no output channel at index %u\n",
				outs[k].idx);
			ret = 1;
			break;
		}
		buf = malloc(size);
		if (!buf) {
			ret = 1;
			break;
		}
		__ane_read(nn, buf, outs[k].idx);
		ret = write_exact(outs[k].path, buf, size);
		free(buf);
	}

	if (!ret && check >= 0) {
		ret = check_program(check, nn, ins, outs);
	}

	ane_free(nn);
	return ret;
}
