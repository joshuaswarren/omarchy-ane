// SPDX-License-Identifier: MIT
// Copyright 2026 Joshua Warren
//
// ane-prog-pack: turn an ANEC program into the program file that ane/h18 reads with hs_prog=1 (request_firmware, no device needed).
//   ane-prog-pack --anec program-0.anec [--in BUFFER_ID=FILE]... -o ane_h18_prog.bin
// It calls the same section builder libane uses for the M2 (ane_m2_program_build), so the six sections are the bytes the T6021 driver loads, and writes them
// with the program's buffer list. --in copies FILE (at most the buffer size) to the start of an input buffer; every other buffer starts zeroed.
// Format (little endian, checked by the module before it allocates anything):
//   +0x00 u32 'ANEP', u32 1, u32 nsec, u32 nio, u64 file size, u64 0
//   +0x20 nsec x 0x20: u32 section id, u32 0, u64 size, u64 offset (16 byte aligned), u64 0
//   then  nio  x 0x20: u32 buffer id, u32 direction (0 input, 1 output, 2 scratch), u64 size, u64 data offset, u64 data size, then the data
// Programs built with a port table (ane_m2_program_build_ports) are not supported here yet.
// Build: cc -O1 -I libane -I ane/src/uapi/drm -o ane-prog-pack tools/ane-prog-pack.c libane/ane_m2.c libane/ane_sha256.c libane/ane.c  (needs drm.h and asm/types.h;
// tools/test_ane_prog_pack.sh builds it with a stub drm.h on a host without libdrm and checks the output against the module's reader rules).
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ane.h"
#include "ane_m2.h"

#define MAGIC 0x50454e41u
#define MAXIN 8

struct infile {
	uint32_t id;
	const char *path;
};

static int read_all(const char *path, uint8_t **out, uint64_t *size)
{
	FILE *f = fopen(path, "rb");
	long n;

	if (!f)
		return -errno;
	if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
		fclose(f);
		return -EIO;
	}
	*out = malloc(n ? (size_t)n : 1);
	if (!*out || fread(*out, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		free(*out);
		return -EIO;
	}
	fclose(f);
	*size = (uint64_t)n;
	return 0;
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

int main(int argc, char **argv)
{
	const char *anec_path = NULL, *out_path = NULL;
	struct infile ins[MAXIN];
	unsigned int nins = 0, i, j, nsec = 0, nio;
	uint8_t *anec = NULL, *blob;
	uint64_t anec_size = 0, pos, total;
	struct ane_m2_model model;
	struct ane_m2_sections secs;
	uint8_t *indata[MAXIN] = { NULL };
	uint64_t insize[MAXIN] = { 0 };
	FILE *f;
	int err;

	for (i = 1; i < (unsigned int)argc; i++) {
		if (!strcmp(argv[i], "--anec") && i + 1 < (unsigned int)argc) {
			anec_path = argv[++i];
		} else if (!strcmp(argv[i], "--chip") && i + 1 < (unsigned int)argc) {
			i++;
			if (!strcmp(argv[i], "h18")) {
				ane_m2_set_chip(ANE_M2_CHIP_H18);
			} else if (strcmp(argv[i], "h14")) {
				fprintf(stderr, "--chip takes h14 or h18\n");
				return 2;
			}
		} else if (!strcmp(argv[i], "-o") && i + 1 < (unsigned int)argc) {
			out_path = argv[++i];
		} else if (!strcmp(argv[i], "--in") && i + 1 < (unsigned int)argc && nins < MAXIN) {
			char *eq = strchr(argv[++i], '=');

			if (!eq) {
				fprintf(stderr, "--in wants BUFFER_ID=FILE\n");
				return 2;
			}
			*eq = 0;
			ins[nins].id = (uint32_t)strtoul(argv[i], NULL, 0);
			ins[nins++].path = eq + 1;
		} else {
			fprintf(stderr, "usage: ane-prog-pack --anec program.anec [--in BUFFER_ID=FILE]... -o prog.bin\n");
			return 2;
		}
	}
	if (!anec_path || !out_path) {
		fprintf(stderr, "usage: ane-prog-pack --anec program.anec [--in BUFFER_ID=FILE]... -o prog.bin\n");
		return 2;
	}
	err = read_all(anec_path, &anec, &anec_size);
	if (err) {
		fprintf(stderr, "cannot read %s: %d\n", anec_path, err);
		return 1;
	}
	memset(&model, 0, sizeof(model));
	memset(&secs, 0, sizeof(secs));
	err = ane_m2_program_build(anec, anec_size, &model, &secs);
	if (err) {
		fprintf(stderr, "ane_m2_program_build failed: %d\n", err);
		return 1;
	}
	for (i = 0; i < ANE_M2_SEC_COUNT; i++)
		if (secs.sec[i].size)
			nsec++;
	nio = model.io_count;
	if (!nsec || !nio || nio > 8) {
		fprintf(stderr, "the program has %u sections and %u buffers, the file format takes 1 to 6 and 1 to 8\n", nsec, nio);
		return 1;
	}
	for (i = 0; i < nins; i++) {
		for (j = 0; j < nio && model.io[j].buffer_id != ins[i].id; j++)
			;
		if (j == nio || model.io[j].dir != 0) {
			fprintf(stderr, "--in %u: not an input buffer of this program\n", ins[i].id);
			return 1;
		}
		err = read_all(ins[i].path, &indata[i], &insize[i]);
		if (err || insize[i] > model.io[j].size) {
			fprintf(stderr, "--in %u: %s unreadable or larger than the buffer (%llu bytes)\n", ins[i].id, ins[i].path, (unsigned long long)model.io[j].size);
			return 1;
		}
	}
	pos = 0x20 + (uint64_t)(nsec + nio) * 0x20;
	total = pos;
	for (i = 0; i < ANE_M2_SEC_COUNT; i++)
		total += ((secs.sec[i].size + 15) & ~15ULL);
	for (i = 0; i < nins; i++)
		total += ((insize[i] + 15) & ~15ULL);
	total = (total + 15) & ~15ULL;
	blob = calloc(1, total);
	if (!blob)
		return 1;
	pos = (pos + 15) & ~15ULL;
	put32(blob, MAGIC);
	put32(blob + 4, 1);
	put32(blob + 8, nsec);
	put32(blob + 12, nio);
	{
		uint8_t *r = blob + 0x20;

		for (i = 0; i < ANE_M2_SEC_COUNT; i++) {
			if (!secs.sec[i].size)
				continue;
			put32(r, ane_m2_section_ids[i]);
			put64(r + 8, secs.sec[i].size);
			put64(r + 16, pos);
			memcpy(blob + pos, secs.sec[i].data, secs.sec[i].size);
			pos += (secs.sec[i].size + 15) & ~15ULL;
			r += 0x20;
		}
		for (i = 0; i < nio; i++, r += 0x20) {
			put32(r, model.io[i].buffer_id);
			put32(r + 4, model.io[i].dir);
			put64(r + 8, model.io[i].size);
			for (j = 0; j < nins; j++)
				if (ins[j].id == model.io[i].buffer_id) {
					put64(r + 16, pos);
					put64(r + 24, insize[j]);
					memcpy(blob + pos, indata[j], insize[j]);
					pos += (insize[j] + 15) & ~15ULL;
				}
			printf("buffer %u dir %u size %llu%s\n", model.io[i].buffer_id, model.io[i].dir, (unsigned long long)model.io[i].size, model.io[i].dir == 0 ? " (input)" : model.io[i].dir == 1 ? " (output)" : " (scratch)");
		}
	}
	put64(blob + 16, total);
	f = fopen(out_path, "wb");
	if (!f || fwrite(blob, 1, total, f) != total || fclose(f)) {
		fprintf(stderr, "cannot write %s\n", out_path);
		return 1;
	}
	printf("wrote %s: %llu bytes, %u sections, %u buffers\n", out_path, (unsigned long long)total, nsec, nio);
	return 0;
}
