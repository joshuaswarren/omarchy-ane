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
//           --out 0=y.fp16 [--repeat N] [--check-add]
//
// Input files must hold at least the channel's allocation bytes (ane_src_size).
// Output files are written with exactly ane_dst_size bytes.
// --check-add verifies y == a + b element-wise against the fp16
// half-away-from-zero reference and needs inputs 0, 1 and output 0.
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

static int check_add(struct ane_nn *nn, struct io_file *in,
		     struct io_file *out)
{
	uint64_t a_size;
	uint64_t b_size;
	uint64_t y_size;
	uint64_t n;
	uint64_t same = 0;
	uint16_t *a;
	uint16_t *b;
	uint16_t *y;

	if (!in[0].set || !in[1].set || !out[0].set || in[0].idx != 0 ||
	    in[1].idx != 1 || out[0].idx != 0) {
		fprintf(stderr, "--check-add needs --in 0, --in 1, --out 0\n");
		return -1;
	}
	a_size = ane_src_size(nn, 0);
	b_size = ane_src_size(nn, 1);
	y_size = ane_dst_size(nn, 0);
	if (!a_size || a_size != b_size || y_size != a_size) {
		fprintf(stderr, "--check-add: io channel sizes disagree\n");
		return -1;
	}
	n = y_size / 2;
	a = read_exact(in[0].path, a_size);
	b = read_exact(in[1].path, b_size);
	y = read_exact(out[0].path, y_size);
	if (!a || !b || !y) {
		free(a);
		free(b);
		free(y);
		return -1;
	}
	for (uint64_t i = 0; i < n; i++) {
		same += ane_f16_add_half_away(a[i], b[i]) == y[i];
	}
	printf("%llu/%llu bit-exact vs half-away reference: %s\n",
	       (unsigned long long)same, (unsigned long long)n,
	       same == n ? "PASS" : "FAIL");
	free(a);
	free(b);
	free(y);
	return same == n ? 0 : -1;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: ane-run --anec FILE [--in IDX=FILE]... "
		"[--out IDX=FILE]... [--repeat N] [--check-add]\n");
}

int main(int argc, char **argv)
{
	const char *anec = NULL;
	struct ane_nn *nn;
	struct io_file ins[8] = { 0 };
	struct io_file outs[8] = { 0 };
	uint32_t repeat = 1;
	int check = 0;
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
		} else if (!strcmp(argv[i], "--check-add")) {
			check = 1;
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

	if (!ret && check) {
		ret = check_add(nn, ins, outs);
	}

	ane_free(nn);
	return ret;
}
