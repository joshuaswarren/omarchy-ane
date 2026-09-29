// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "ane.h"
#include "ane_m2.h"
#include "ane_f16_add.h"

/*
// HOST-ONLY self-check: no device, no kernel, no hardware anywhere.
//
// 1. Byte identity: ane_m2_program_build() on fixtures/program-0.anec must
//    reproduce the proven Python lab section builder's output byte for byte
//    (fixtures generic/kernel/descriptor/operation/procedure/tdprop bins --
//    the exact payloads that produced the bit-exact y == a+b run through
//    the scratch-module sequencer path). This is the main proof of the
//    builder; it says nothing about device behaviour.
// 2. The fp16 half-away reference vectors (independent of numpy: the ANE's
//    add rounding, verified against a numpy ties-away oracle when the
//    vectors were pinned).
// 3. Envelope refusals: a truncated anec must be rejected.
//
// usage: ane-selfcheck [fixtures-dir]
*/

static const char *fixture(const char *dir, const char *name)
{
	static char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	return path;
}

static int check_section(const char *dir, const char *name, int which,
			 const struct ane_m2_sections *secs)
{
	const char *path = fixture(dir, name);
	FILE *fp = fopen(path, "rb");
	long expect;
	int ok;

	if (!fp) {
		printf("FAIL %s: cannot open\n", path);
		return 0;
	}
	fseek(fp, 0, SEEK_END);
	expect = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	ok = expect > 0 && (uint64_t)expect == secs->sec[which].size;
	if (ok) {
		void *buf = malloc((uint64_t)expect);
		if (fread(buf, 1, (uint64_t)expect, fp) != (size_t)expect) {
			ok = 0;
		}
		ok = ok && !memcmp(buf, secs->sec[which].data,
				   secs->sec[which].size);
		free(buf);
	}
	fclose(fp);
	printf("  [%s] %s (%ld bytes byte-identical)\n", ok ? "ok" : "FAIL",
	       name, expect);
	return ok;
}

static int check_byte_identity(const char *dir)
{
	static const struct {
		const char *name;
		int which;
	} table[ANE_M2_SEC_COUNT] = {
		{ "generic.bin", ANE_M2_SEC_GENERIC },
		{ "kernel.bin", ANE_M2_SEC_KERNEL },
		{ "descriptor.bin", ANE_M2_SEC_DESCRIPTOR },
		{ "operation.bin", ANE_M2_SEC_OPERATION },
		{ "procedure.bin", ANE_M2_SEC_PROCEDURE },
		{ "tdprop.bin", ANE_M2_SEC_TDPROP },
	};
	const char *anec_path = fixture(dir, "program-0.anec");
	FILE *fp = fopen(anec_path, "rb");
	uint8_t *anec;
	struct ane_m2_model model;
	struct ane_m2_sections secs;
	long size;
	int err;
	int ok;
	int i;

	if (!fp) {
		printf("FAIL %s: cannot open\n", anec_path);
		return 0;
	}
	fseek(fp, 0, SEEK_END);
	size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	anec = malloc((uint64_t)size);
	if (fread(anec, 1, (uint64_t)size, fp) != (size_t)size) {
		fclose(fp);
		free(anec);
		printf("FAIL %s: short read\n", anec_path);
		return 0;
	}
	fclose(fp);

	err = ane_m2_program_build(anec, (uint64_t)size, &model, &secs);
	free(anec);
	if (err) {
		printf("FAIL ane_m2_program_build: %d\n", err);
		return 0;
	}

	ok = 1;
	printf("byte identity vs the Python builder (%s):\n", dir);
	for (i = 0; i < ANE_M2_SEC_COUNT; i++) {
		ok = check_section(dir, table[i].name, table[i].which, &secs) &&
		     ok;
	}
	ane_m2_sections_free(&secs);
	return ok;
}

static int check_f16_add(void)
{
	/* Pinned vectors: {a, b, expected}. Expected values were verified
	 * against the numpy ties-away oracle; ties, binade steps, subnormal
	 * and overflow edges included. */
	static const uint16_t v[][3] = {
		{ 0x0000, 0x0000, 0x0000 }, /* +0 + +0 */
		{ 0x8000, 0x8000, 0x8000 }, /* -0 + -0 */
		{ 0x8000, 0x0000, 0x0000 }, /* -0 + +0 */
		{ 0x3c00, 0x3c00, 0x4000 }, /* 1 + 1 = 2 */
		{ 0x3800, 0x0c00, 0x3801 }, /* 0.5 + 2^-12: exact tie, away up */
		{ 0xc000, 0xc000, 0xc400 }, /* -2 + -2 = -4 */
		{ 0x03ff, 0x0001, 0x0400 }, /* max + min subnormal = 2^-14 */
		{ 0x0400, 0x0001, 0x0401 }, /* 2^-14 + 2^-24, exact */
		{ 0x03ff, 0x8001, 0x03fe }, /* 1023 - 1 quanta */
		{ 0x7bff, 0x5000, 0x7c00 }, /* 65504 + 16: tie away to inf */
		{ 0x7bff, 0x4800, 0x7bff }, /* 65504 + 8: rounds back down */
		{ 0x7bff, 0xdc00, 0x7bf7 }, /* 65504 - 256 = 65248 */
		{ 0x0001, 0x0001, 0x0002 }, /* two min subnormals */
		{ 0x7e00, 0x3c00, 0x7e00 }, /* NaN propagate */
		{ 0x7c00, 0xc000, 0x7c00 }, /* inf + -2 */
		{ 0x7c00, 0xfc00, 0x7e00 }, /* inf + -inf = NaN */
		{ 0xbc00, 0x4000, 0x3c00 }, /* -1 + 2 = 1 */
		{ 0x3555, 0xb555, 0x0000 }, /* x + -x = +0 */
	};
	int ok = 1;
	printf("fp16 half-away add vectors:\n");
	for (unsigned long i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
		uint16_t got = ane_f16_add_half_away(v[i][0], v[i][1]);
		int good = got == v[i][2];
		ok = ok && good;
		printf("  [%s] %04x + %04x = %04x (want %04x)\n",
		       good ? "ok" : "FAIL", v[i][0], v[i][1], got, v[i][2]);
	}
	return ok;
}

static int check_refusals(const char *dir)
{
	const char *anec_path = fixture(dir, "program-0.anec");
	FILE *fp = fopen(anec_path, "rb");
	uint8_t *anec;
	struct ane_m2_model model;
	struct ane_m2_sections secs;
	long size;
	int ok = 1;
	int err;
	int good;

	printf("envelope refusals:\n");
	if (!fp) {
		printf("  [FAIL] %s: cannot open\n", anec_path);
		return 0;
	}
	fseek(fp, 0, SEEK_END);
	size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	anec = malloc((uint64_t)size);
	if (fread(anec, 1, (uint64_t)size, fp) != (size_t)size) {
		fclose(fp);
		free(anec);
		return 0;
	}
	fclose(fp);

	/* Truncated payload must be refused. */
	err = ane_m2_program_build(anec, (uint64_t)size / 2, &model, &secs);
	good = err != 0;
	ok = ok && good;
	printf("  [%s] truncated anec refused\n", good ? "ok" : "FAIL");

	/* A tampered task length must be refused. */
	anec[0x08] ^= 0x04; /* firstTaskBytes 244 -> 248, breaks tsk_size */
	err = ane_m2_program_build(anec, (uint64_t)size, &model, &secs);
	good = err != 0;
	ok = ok && good;
	printf("  [%s] tampered td_size refused\n", good ? "ok" : "FAIL");

	free(anec);
	return ok;
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "fixtures";
	int ok;

	printf("HOST-ONLY self-check (no device; proves nothing about "
	       "hardware behaviour)\n");
	ok = check_byte_identity(dir);
	ok = check_f16_add() && ok;
	ok = check_refusals(dir) && ok;

	printf("%s\n", ok ? "SELF-CHECK PASS" : "SELF-CHECK FAIL");
	return ok ? 0 : 1;
}
