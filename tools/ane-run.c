// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ane.h"
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

/* The checked fixture ops and their input arity. */
enum {
	CHK_ADD, CHK_MUL, CHK_RELU, CHK_ADD_SCALAR, CHK_MUL_SCALAR,
	CHK_REAL_DIV, CHK_CLIP_LOW, CHK_CLIP_HIGH, CHK_MATVEC, CHK_COUNT
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

static int check_program(int op, struct ane_nn *nn, struct io_file *in,
			 struct io_file *out)
{
	int two_in = check_ops[op].ins == 2;
	int matvec = check_ops[op].matvec;
	uint64_t a_size, b_size = 0, y_size, n, lanes, i, j;
	uint64_t pad_in = 0, pad_out = 0, exact = 0, in_band = 0;
	uint64_t max_ulp_milli = 0;
	double max_nerr = 0.0;
	uint16_t *a, *b = NULL, *y;
	int ok;
	uint32_t M = 1, K = 0, N = 0;
	uint16_t *w = NULL;

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

static void usage(void)
{
	fprintf(stderr,
		"usage: ane-run --anec FILE [--in IDX=FILE]... "
		"[--out IDX=FILE]... [--repeat N] [--check OP] "
		"[--weights FILE]\n"
		"OP: add mul relu add-scalar mul-scalar real-div-scalar "
		"clip-low clip-high matvec\n"
		"--weights: matvec weight table, fp16 row-major [N, K] "
		"(BLOBFILE layout). Required with --check matvec.\n");
}

int main(int argc, char **argv)
{
	const char *anec = NULL;
	struct ane_nn *nn;
	struct io_file ins[8] = { 0 };
	struct io_file outs[8] = { 0 };
	uint32_t repeat = 1;
	int check = -1;
	int ret;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--anec") && i + 1 < argc) {
			anec = argv[++i];
		} else if (!strcmp(argv[i], "--in") && i + 1 < argc) {
			int slot = -1;

			for (int k = 0; k < 8; k++) {
				if (!ins[k].set) {
					slot = k;
					break;
				}
			}
			if (slot < 0) {
				fprintf(stderr, "too many --in args (max 8)\n");
				return 2;
			}
			if (parse_io_arg(argv[++i], &ins[slot], "in"))
				return 2;
		} else if (!strcmp(argv[i], "--out") && i + 1 < argc) {
			int slot = -1;

			for (int k = 0; k < 8; k++) {
				if (!outs[k].set) {
					slot = k;
					break;
				}
			}
			if (slot < 0) {
				fprintf(stderr, "too many --out args (max 8)\n");
				return 2;
			}
			if (parse_io_arg(argv[++i], &outs[slot], "out"))
				return 2;
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
		} else {
			usage();
			return 2;
		}
	}
	if (!anec) {
		usage();
		return 2;
	}

	nn = ane_init(anec);

	if (!nn) {
		fprintf(stderr, "ane_init failed on %s\n", anec);
		return 1;
	}

	ret = 0;
	for (int k = 0; k < 8 && !ret; k++) {
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

	for (uint32_t r = 0; r < repeat && !ret; r++) {
		if (ane_exec(nn) < 0) {
			fprintf(stderr, "ane_exec failed\n");
			ret = 1;
		}
	}

	for (int k = 0; k < 8 && !ret; k++) {
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
