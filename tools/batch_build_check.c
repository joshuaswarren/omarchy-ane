// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

/* Offline driver-parser proof for the N-add batch packages.
 *
 * Runs the SHIPPED ane_m2_program_build() - the exact parser the M2
 * driver's userspace path executes - over each package and prints what
 * it derives: the io channels, the union BAR ref set, and the call
 * record count. A package the M2 would refuse fails here, on the CT,
 * before any hardware time is spent. Host-usable by contract
 * (libane/ane_m2.h: "no device, no fd").
 *
 * usage: batch_build_check PACKAGE.anec [PACKAGE.anec ...]
 * exit 0 when every package builds and every derivation matches the
 * expected shape; nonzero otherwise.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ane_m2.h"

#define EXPECT_REFS 3

static void *read_all(const char *path, uint64_t *size_out)
{
	FILE *f = fopen(path, "rb");
	long len;
	void *buf;

	if (!f || fseek(f, 0, SEEK_END) || (len = ftell(f)) < 0 ||
	    fseek(f, 0, SEEK_SET)) {
		return NULL;
	}
	buf = malloc((size_t)len);
	if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*size_out = (uint64_t)len;
	return buf;
}

static int fail_one(const char *path, int *fails, const char *why)
{
	printf("%s: FAIL %s\n", path, why);
	(*fails)++;
	return -1;
}

static int check_one(const char *path, uint32_t tiles_exp, int *fails)
{
	struct ane_m2_model model;
	struct ane_m2_sections secs;
	const uint8_t *anec;
	uint64_t size;
	uint32_t tiles[7], i;
	int err = 0;

	anec = read_all(path, &size);
	if (!anec) {
		return fail_one(path, fails, "unreadable");
	}
	if (size < 0x1000 + 7 * 4) {
		err = fail_one(path, fails, "short file");
		goto out;
	}
	memcpy(tiles, anec + 0x28, sizeof(tiles));
	if (tiles[4] != tiles_exp || tiles[5] != tiles_exp ||
	    tiles[6] != tiles_exp) {
		err = fail_one(path, fails, "io tile counts wrong");
		goto out;
	}

	memset(&model, 0, sizeof(model));
	memset(&secs, 0, sizeof(secs));
	if (ane_m2_program_build(anec, size, &model, &secs)) {
		err = fail_one(path, fails, "ane_m2_program_build refused");
		goto out;
	}
	printf("%s: io %u refs %u calls %u\n", path, model.io_count,
	       model.call_ref_count[0], model.calls);
	for (i = 0; i < model.io_count; i++) {
		printf("  io[%u] buffer_id %u dir %u size %llu\n", i,
		       model.io[i].buffer_id, model.io[i].dir,
		       (unsigned long long)model.io[i].size);
	}
	for (i = 0; i < model.call_ref_count[0]; i++) {
		printf("  ref slot %u -> tag %u\n", model.call_refs[0][i].slot,
		       model.call_refs[0][i].tag);
	}

	if (model.io_count != 3 || model.io[0].buffer_id != 5 ||
	    model.io[1].buffer_id != 6 || model.io[2].buffer_id != 4 ||
	    model.io[0].dir != 0 || model.io[1].dir != 0 ||
	    model.io[2].dir != 1 ||
	    model.io[0].size != (uint64_t)tiles_exp << 14 ||
	    model.io[1].size != model.io[0].size ||
	    model.io[2].size != model.io[0].size) {
		err = fail_one(path, fails, "io table is not ch5/ch6/ch4 at "
					   "tiles_exp << 14");
		goto out;
	}
	if (model.calls != 1 || model.call_ref_count[0] != EXPECT_REFS) {
		err = fail_one(path, fails,
			       "call/ref union is not the single-record add");
		goto out;
	}
	for (i = 0; i < model.call_ref_count[0]; i++) {
		uint32_t slot = model.call_refs[0][i].slot;
		uint32_t tag = model.call_refs[0][i].tag;
		uint32_t want = slot == 4 ? 5 : slot == 5 ? 4 : slot == 6 ? 6
									  : 0;

		if (slot != 4 && slot != 5 && slot != 6) {
			err = fail_one(path, fails, "unexpected ref slot");
			goto out;
		}
		if (tag != want) {
			err = fail_one(path, fails,
				       "ref tag != the proven add union");
			goto out;
		}
	}
	printf("%s: derivation matches the proven single-add shape\n", path);
out:
	free((void *)anec);
	*fails += err ? 0 : 0;
	return err;
}

int main(int argc, char **argv)
{
	int fails = 0;

	for (int i = 1; i < argc; i++) {
		const char *p = argv[i];
		uint32_t n = 0;
		const char *tag = strstr(p, "add-batch-");
		const char *d;

		if (tag) {
			for (d = tag + strlen("add-batch-");
			     *d >= '0' && *d <= '9'; d++) {
				n = n * 10 + (uint32_t)(*d - '0');
			}
		}
		if (!n) {
			printf("%s: FAIL no add-batch-N in path\n", p);
			fails++;
			continue;
		}
		check_one(p, 2 * n, &fails);
	}
	return fails ? 1 : 0;
}
