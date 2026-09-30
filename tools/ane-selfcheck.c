// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ane.h"
#include "ane_m2.h"
#include "ane_f16_add.h"

/*
// HOST-ONLY self-check: no device, no kernel, no hardware anywhere.
//
// 1. Byte identity: ane_m2_program_build() on every fixture under
//    fixtures/h14-anec/<op> must reproduce the lab Python section builder
//    (tools/h14_sections.py) byte for byte: the six <section>.bin files
//    next to each ANEC. For add those payloads are additionally the exact
//    bytes of the hardware-proven LOAD command. This proves the builder;
//    it says nothing about device behaviour.
// 2. The fp16 half-away add reference vectors (independent of numpy: the
//    ANE's add rounding, verified against a numpy ties-away oracle when
//    the vectors were pinned).
// 3. Envelope refusals: one test per refusal the lab builder raises
//    (fixtures/h14-anec README in the lab repo), plus the truncated and
//    tampered-firstTaskBytes cases. Every corruption is a named byte
//    patch on a real fixture.
//
// usage: ane-selfcheck [fixtures-dir]   (default ../fixtures/h14-anec)
*/

static const char *fixture(const char *dir, const char *op, const char *name)
{
	static char path[512];

	snprintf(path, sizeof(path), "%s/%s/%s", dir, op, name);
	return path;
}

static uint8_t *read_all(const char *path, long *out_size)
{
	FILE *fp = fopen(path, "rb");
	uint8_t *buf;
	long size;

	if (!fp) {
		printf("FAIL %s: cannot open\n", path);
		return NULL;
	}
	fseek(fp, 0, SEEK_END);
	size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	buf = malloc((uint64_t)size);
	if (!buf || fread(buf, 1, (uint64_t)size, fp) != (size_t)size) {
		printf("FAIL %s: short read\n", path);
		free(buf);
		fclose(fp);
		return NULL;
	}
	fclose(fp);
	*out_size = size;
	return buf;
}

static int check_byte_identity(const char *dir, const char *op)
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
	const char *anec_path = fixture(dir, op, "program-0.anec");
	uint8_t *anec;
	struct ane_m2_model model;
	struct ane_m2_sections secs;
	long size;
	int err;
	int ok;
	int i;

	anec = read_all(anec_path, &size);
	if (!anec) {
		return 0;
	}
	err = ane_m2_program_build(anec, (uint64_t)size, &model, &secs);
	free(anec);
	if (err) {
		printf("  [FAIL] %s: ane_m2_program_build: %d\n", op, err);
		return 0;
	}
	ok = 1;
	for (i = 0; i < ANE_M2_SEC_COUNT; i++) {
		const char *path = fixture(dir, op, table[i].name);
		const struct ane_m2_sections *s = &secs;
		uint8_t *want = read_all(path, &size);
		int good;

		if (!want) {
			ane_m2_sections_free(&secs);
			return 0;
		}
		good = (uint64_t)size == s->sec[table[i].which].size &&
		       !memcmp(want, s->sec[table[i].which].data,
			       (uint64_t)size);
		if (!good) {
			printf("  [FAIL] %s: %s differs (%ld B want, %llu B "
			       "got)\n", op, table[i].name, size,
			       (unsigned long long)s->sec[table[i].which].size);
		}
		free(want);
		ok = ok && good;
	}
	ane_m2_sections_free(&secs);
	printf("  [%s] %s: six sections byte-identical\n", ok ? "ok" : "FAIL",
	       op);
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

/* One refusal test: load <op>/program-0.anec, mutate it, expect refusal. */
static int check_refusal(const char *dir, const char *op, const char *what,
			 void (*mutate)(uint8_t *anec, long *size))
{
	const char *anec_path = fixture(dir, op, "program-0.anec");
	uint8_t *anec;
	struct ane_m2_model model;
	struct ane_m2_sections secs;
	long size;
	int err;
	int good;

	anec = read_all(anec_path, &size);
	if (!anec) {
		return 0;
	}
	mutate(anec, &size);
	err = ane_m2_program_build(anec, (uint64_t)size, &model, &secs);
	good = err != 0;
	printf("  [%s] %s\n", good ? "ok" : "FAIL", what);
	free(anec);
	return good;
}

static void mut_truncated(uint8_t *a, long *size)
{
	(void)a;
	*size /= 2; /* payload shorter than the header promises */
}

static void mut_td_size(uint8_t *a, long *size)
{
	(void)size;
	a[0x08] ^= 0x04; /* firstTaskBytes 244 -> 248, not the walked task */
}

static void mut_version(uint8_t *a, long *size)
{
	(void)size;
	a[0x24] ^= 0x04; /* header version word 1 -> 5 */
}

static void mut_input_count(uint8_t *a, long *size)
{
	(void)size;
	a[0x20] = 4; /* inputCount 4: no derivation for channels 8+ */
}

static void mut_task_words(uint8_t *a, long *size)
{
	(void)size;
	a[0x1013] |= 0x07; /* task header declares ~7400 B in a 260 B stream */
}

static void mut_task_gap(uint8_t *a, long *size)
{
	(void)size;
	a[0x10a8] = 0x01; /* nonzero byte in the real-div task-0 gap */
}

static void mut_bar_reg(uint8_t *a, long *size)
{
	(void)size;
	a[0x10e0] = 0x40; /* add src-base record 0x1110 -> 0x1100 */
}

static void mut_bar_slot(uint8_t *a, long *size)
{
	(void)size;
	/* matvec task 1's slot-1 BAR-ref at base 0x1908 (KernelDMA, tag 2)
	 * -> base 0x1108 (in the surface range; falls through both rules to
	 * "BAR-ref record outside the known register roles"). Both rules
	 * refuse. */
	a[0x11f9] = (a[0x11f9] & 0x80) | 0x04;
}

static void mut_empty_stream(uint8_t *a, long *size)
{
	(void)size;
	memset(a + 0x1000, 0, 0xcc); /* relu stream: every word a filler */
	a[0x0c] = 0;
	a[0x0d] = 0;
	a[0x0e] = 0;
	a[0x0f] = 0; /* taskCount 0 */
}

/* Island ANECs (island-c-pv, island-a-kt, island-a-attn-p1,
 * island-b-select-runtime, rms-c2048-gamma) reuse BAR slots across tasks
 * with different tags. fw135.3 pushToHWDirect has no per-task BAR walk
 * (the 61-slot patch table at netDesc+0xC is global per call,
 * 0x44e58-0x44ea4), so the operation section for these programs cannot
 * be expressed under the single-record model that pushToHWDirect reads.
 * The C builder refuses them at build time with the cross-task slot
 * conflict diagnostic. The procedure section is unchanged (tot=1,
 * contentType=3, rowIdx=0); the islands are blocked by the BAR table,
 * not by anything else. This test loads each island and asserts the
 * refusal. */
static int check_island_refusal(const char *dir, const char *op)
{
	const char *anec_path = fixture(dir, op, "program-0.anec");
	struct ane_m2_model model;
	struct ane_m2_sections secs;
	uint8_t *anec;
	long size;
	int err;
	int good;

	anec = read_all(anec_path, &size);
	if (!anec) {
		return 0;
	}
	err = ane_m2_program_build(anec, (uint64_t)size, &model, &secs);
	good = err != 0;
	ane_m2_sections_free(&secs);
	free(anec);
	printf("  [%s] %s: cross-task BAR-slot conflict refused\n",
	       good ? "ok" : "FAIL", op);
	return good;
}

int main(int argc, char **argv)
{
	static const char *const ops[] = {
		"add", "mul", "relu", "add-scalar", "mul-scalar",
		"real-div-scalar", "clip-low", "clip-high", "matvec",
		/* rms-c2048-gamma fits global-unique slots under the legacy
		 * rule (slot<=1 -> tag 2 regardless of base register); it
		 * builds to the same bytes the fixture holds. The legacy
		 * rule silently rewrites t7's input ch6 read (base 0x1128)
		 * to kernel-base, which is semantically wrong but produces
		 * a global-table-consistent section. The fixture files are
		 * the lab's record of that legacy output; an island test
		 * that exercises the rms ANEC under the BASE rule would
		 * refuse it (cross-task conflict). */
		"rms-c2048-gamma",
	};
	static const char *const islands[] = {
		/* island-c-pv, island-a-kt, island-a-attn-p1 and
		 * island-b-select-runtime reuse BAR slot 3 (or 1/7 in
		 * b-select) with different tags across tasks. The base
		 * rule and the legacy rule both refuse them with the
		 * "global unique slots" diagnostic. rms-c2048-gamma is
		 * NOT in this list: under the legacy rule its tags happen
		 * to be consistent, so the C builder accepts it; the
		 * legacy-rule semantics are documented above. */
		"island-c-pv", "island-a-kt", "island-a-attn-p1",
		"island-b-select-runtime",
	};
	const char *dir = argc > 1 ? argv[1] : "../fixtures/h14-anec";
	int ok = 1;
	unsigned i;

	printf("HOST-ONLY self-check (no device; proves nothing about "
	       "hardware behaviour)\n");
	printf("byte identity vs the Python builder (%s):\n", dir);
	for (i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
		ok = check_byte_identity(dir, ops[i]) && ok;
	}
	ok = check_f16_add() && ok;

	printf("island refusal (cross-task BAR-slot conflict, "
	       "fw135.3 pushToHWDirect is global per call):\n");
	for (i = 0; i < sizeof(islands) / sizeof(islands[0]); i++) {
		ok = check_island_refusal(dir, islands[i]) && ok;
	}

	printf("envelope refusals:\n");
	ok = check_refusal(dir, "add", "truncated anec refused",
			   mut_truncated) && ok;
	ok = check_refusal(dir, "add", "tampered firstTaskBytes refused",
			   mut_td_size) && ok;
	ok = check_refusal(dir, "add", "unknown header version refused",
			   mut_version) && ok;
	ok = check_refusal(dir, "add", "inputCount 4 refused",
			   mut_input_count) && ok;
	ok = check_refusal(dir, "add", "task beyond the stream refused",
			   mut_task_words) && ok;
	ok = check_refusal(dir, "real-div-scalar",
			   "nonzero 16-byte task gap refused",
			   mut_task_gap) && ok;
	ok = check_refusal(dir, "add", "BAR-ref at an unknown register refused",
			   mut_bar_reg) && ok;
	ok = check_refusal(dir, "matvec",
			   "BAR-ref at a between-bases register refused",
			   mut_bar_slot) && ok;
	ok = check_refusal(dir, "relu", "empty task stream refused",
			   mut_empty_stream) && ok;

	printf("%s\n", ok ? "SELF-CHECK PASS" : "SELF-CHECK FAIL");
	return ok ? 0 : 1;
}
