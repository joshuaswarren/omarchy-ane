// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

/*
// Host-only check of the role-to-channel derivation. Needs no ANE: it builds
// task images in memory, and reads .anec files straight off disk.
//
//   ./main.out --self-test          built-in cases, no artifacts needed
//   ./main.out FILE.anec...         one line per file:
//     PATH derived=0|1 src=a,b dst=c positional_src=a,b positional_dst=c
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ane.h"
#include "ane_bind.h"

#define ANEC_HEADER_SIZE 0x1000UL

/* struct anec is declared all-const, so tests build the same layout writable
 * and copy it in. */
struct anec_writable {
	uint64_t size;
	uint32_t td_size;
	uint32_t td_count;
	uint64_t tsk_size;
	uint64_t krn_size;
	uint32_t src_count;
	uint32_t dst_count;
	uint32_t tiles[TILE_COUNT];
	uint64_t nchw[TILE_COUNT][6];
} __attribute__((__packed__, aligned(1)));

_Static_assert(sizeof(struct anec_writable) == sizeof(struct anec),
	       "test header mirror must match struct anec");

/* One minimal H13 task image: ten header words carrying the selector word,
 * then a one-word register record per DMA configuration register. */
struct task_image {
	uint8_t bytes[128];
	uint64_t size;
};

static int failures;

static void expect(int condition, const char *what)
{
	printf("%s %s\n", condition ? "ok  " : "FAIL", what);
	if (!condition)
		failures++;
}

static void put32(uint8_t *at, uint32_t value)
{
	at[0] = (uint8_t)value;
	at[1] = (uint8_t)(value >> 8);
	at[2] = (uint8_t)(value >> 16);
	at[3] = (uint8_t)(value >> 24);
}

static void build_task(struct task_image *task, uint32_t selectors,
		       uint32_t src1_cfg, uint32_t src2_cfg, uint32_t dst_cfg)
{
	static const uint32_t registers[3] = { 0x13800u, 0x13804u, 0x17800u };
	uint32_t configs[3];
	uint64_t at = 40;
	int slot;

	configs[0] = src1_cfg;
	configs[1] = src2_cfg;
	configs[2] = dst_cfg;

	memset(task, 0, sizeof(*task));
	put32(task->bytes + 8 * 4, selectors);

	for (slot = 0; slot != 3; slot++) {
		put32(task->bytes + at, registers[slot]);
		put32(task->bytes + at + 4, configs[slot]);
		at += 8;
	}
	/* No trailing word: a real task ends on the last word of its last
	 * register record, which is what the walk's bound check expects. */
	task->size = at;
}

static void build_header(uint8_t *raw, uint32_t src_count, uint32_t dst_count,
			 const uint32_t *channels, uint32_t channel_count,
			 uint64_t stream_size)
{
	struct anec_writable writable;
	uint32_t index;

	memset(&writable, 0, sizeof(writable));
	writable.size = stream_size;
	writable.td_size = (uint32_t)stream_size;
	writable.td_count = 1;
	writable.tsk_size = stream_size;
	writable.src_count = src_count;
	writable.dst_count = dst_count;
	for (index = 0; index != channel_count; index++)
		writable.tiles[channels[index]] = 1;

	memcpy(raw, &writable, sizeof(writable));
}

#define as_anec(raw) ((const struct anec *)(raw))

/* Append one single-register record to a task image. */
static void add_record(struct task_image *task, uint32_t reg, uint32_t value)
{
	put32(task->bytes + task->size, reg);
	put32(task->bytes + task->size + 4, value);
	task->size += 8;
}

/* The staged-Qwen defect: prog_000's conv-state output is a 393216-byte
 * surface the task writes through its destination DMA, while the old header
 * gave that channel one 16 KB tile. */
static void overrun_tests(void)
{
	static const uint32_t two[2] = { 4, 5 };
	static const uint64_t conv_state[6] = { 1, 1, 6144, 3, 393216, 64 };
	static const uint64_t beta[6] = { 1, 16, 1, 1, 64, 64 };
	static const uint64_t mask[6] = { 1, 1, 1, 50, 128, 128 };
	static const uint64_t bool_select[6] = { 1, 8, 375, 375, 144000, 384 };
	const uint32_t src_on = 0x00033881u;
	const uint32_t dst_on = 0x040000c1u;
	struct task_image task;
	uint8_t anec[sizeof(struct anec)];
	uint32_t channel = 0;
	uint64_t need = 0;
	uint64_t have = 0;

	build_task(&task, 0x00025864u, src_on, ANE_BIND_DMA_DISABLED, dst_on);
	add_record(&task, 0x17810u, 0x60000u);
	add_record(&task, 0x17814u, 0x60000u);
	build_header(anec, 1, 1, two, 2, task.size);
	expect(ane_bind_overrun(as_anec(anec), task.bytes, task.size, 14,
				&channel, &need, &have) == 1 &&
		       channel == 5 && need == 0x60000 && have == 0x4000,
	       "a 384 KB destination write into a 16 KB channel is refused");

	build_task(&task, 0x00025864u, src_on, ANE_BIND_DMA_DISABLED, dst_on);
	add_record(&task, 0x13814u, 0x4000u);
	add_record(&task, 0x17810u, 0x4000u);
	build_header(anec, 1, 1, two, 2, task.size);
	expect(ane_bind_overrun(as_anec(anec), task.bytes, task.size, 14,
				&channel, &need, &have) == 0,
	       "transfers that fill their channels exactly fit");

	build_task(&task, 0x00025864u, src_on, ANE_BIND_DMA_DISABLED,
		   ANE_BIND_DMA_DISABLED);
	add_record(&task, 0x13814u, 0x8000u);
	build_header(anec, 1, 1, two, 2, task.size);
	expect(ane_bind_overrun(as_anec(anec), task.bytes, task.size, 14,
				&channel, &need, &have) == 1 &&
		       channel == 4 && need == 0x8000,
	       "a source-1 read past its channel is refused");

	/* Host side: the bytes ane_tile/ane_untile touch for a header geometry,
	 * which __ane_tile_send/__ane_tile_read compare with the mapping. */
	expect(ane_bind_tile_span(conv_state) == 393216,
	       "a 64 B-row conv state spans its full packed size");
	expect(ane_bind_tile_span(beta) == 1024,
	       "a 64 B-plane [16,1,1] surface spans 16 planes");
	expect(ane_bind_tile_span(mask) == 128,
	       "a single padded row spans its row stride");
	expect(ane_bind_tile_span(bool_select) == UINT64_MAX,
	       "a 1-byte bool surface cannot be fp16-tiled");
}

static void self_test(void)
{
	static const uint32_t three[3] = { 4, 5, 6 };
	static const uint32_t two[2] = { 4, 5 };
	const uint32_t src_on = 0x00033881u;
	const uint32_t dst_on = 0x040000c1u;
	struct task_image task;
	uint8_t anec[sizeof(struct anec)];
	struct ane_bind bind;

	/* The proven H13 family puts its destination on channel 4 and its
	 * sources on 5 and 6. Two proven artifacts carry the three selector
	 * fields in opposite order (0x00024966 and 0x000249a5), so field
	 * position cannot be the role and both must derive the same map. */
	build_task(&task, 0x00024966u, src_on, src_on, dst_on);
	build_header(anec, 2, 1, three, 3, task.size);
	expect(ane_bind_init(as_anec(anec), task.bytes, task.size, &bind) &&
		       bind.dst[0] == 4 && bind.src[0] == 5 && bind.src[1] == 6,
	       "field order 6,5,4 derives dst=4 src=5,6");

	build_task(&task, 0x000249a5u, src_on, src_on, dst_on);
	expect(ane_bind_init(as_anec(anec), task.bytes, task.size, &bind) &&
		       bind.dst[0] == 4 && bind.src[0] == 5 && bind.src[1] == 6,
	       "field order 5,6,4 derives the same dst=4 src=5,6");

	/* Apple's own allocation, as exported through CoreML: the reverse of
	 * the positional layout. Source 2 selects channel 1, the kernel BAR,
	 * which is not a runtime surface and takes no role. */
	build_task(&task, 0x00025864u, src_on, src_on, dst_on);
	build_header(anec, 1, 1, two, 2, task.size);
	expect(ane_bind_init(as_anec(anec), task.bytes, task.size, &bind) &&
		       bind.dst[0] == 5 && bind.src[0] == 4,
	       "Apple selector 0x25864 derives dst=5 src=4, not the reverse");

	/* Real output writes carry more than one destination configuration. */
	build_task(&task, 0x00025864u, src_on, src_on, 0x000000c1u);
	expect(ane_bind_init(as_anec(anec), task.bytes, task.size, &bind) &&
		       bind.dst[0] == 5 && bind.src[0] == 4,
	       "destination config 0x000000c1 is a real output write");

	/* A surface the task stream never names binds on the first unused
	 * allocated channel, destinations first (b0028cd). */
	build_task(&task, 0x00025864u, src_on, ANE_BIND_DMA_DISABLED,
		   ANE_BIND_DMA_DISABLED);
	expect(ane_bind_init(as_anec(anec), task.bytes, task.size, &bind) &&
		       bind.dst[0] == 5 && bind.src[0] == 4,
	       "an unnamed surface binds on the next allocated channel");

	/* With no allocated channel left for it, the counts stay unmet and
	 * the positional layout stands rather than half a map. */
	build_header(anec, 1, 1, two, 1, task.size);
	expect(!ane_bind_init(as_anec(anec), task.bytes, task.size, &bind) &&
		       bind.dst[0] == 4 && bind.src[0] == 5,
	       "an unplaceable surface keeps the positional layout");
	build_header(anec, 1, 1, two, 2, task.size);

	/* A truncated task is refused, not walked off the end. */
	build_task(&task, 0x00025864u, src_on, src_on, dst_on);
	expect(!ane_bind_init(as_anec(anec), task.bytes, 8, &bind) &&
		       bind.dst[0] == 4,
	       "a task shorter than its own header is refused");

	overrun_tests();
}

static int report(const char *path)
{
	struct anec anec;
	struct ane_bind bind;
	struct ane_bind positional;
	unsigned char *stream;
	uint32_t index;
	int derived;
	int overrun;
	uint32_t channel;
	uint64_t need;
	uint64_t have;
	FILE *file = fopen(path, "rb");

	if (!file) {
		fprintf(stderr, "cannot open %s\n", path);
		return 1;
	}
	if (fread(&anec, 1, sizeof(anec), file) != sizeof(anec) || !anec.size) {
		fprintf(stderr, "not an anec: %s\n", path);
		fclose(file);
		return 1;
	}
	stream = malloc(anec.size);
	if (!stream || fseek(file, ANEC_HEADER_SIZE, SEEK_SET) ||
	    fread(stream, 1, anec.size, file) != anec.size) {
		fprintf(stderr, "short read: %s\n", path);
		free(stream);
		fclose(file);
		return 1;
	}
	fclose(file);

	derived = ane_bind_init(&anec, stream, anec.size, &bind);
	ane_bind_positional(&anec, &positional);
	overrun = ane_bind_overrun(&anec, stream, anec.size, 14, &channel, &need,
				   &have);
	free(stream);

	printf("%s derived=%d src=", path, derived);
	for (index = 0; index != anec.src_count; index++)
		printf("%s%u", index ? "," : "", bind.src[index]);
	printf(" dst=");
	for (index = 0; index != anec.dst_count; index++)
		printf("%s%u", index ? "," : "", bind.dst[index]);
	printf(" positional_src=");
	for (index = 0; index != anec.src_count; index++)
		printf("%s%u", index ? "," : "", positional.src[index]);
	printf(" positional_dst=");
	for (index = 0; index != anec.dst_count; index++)
		printf("%s%u", index ? "," : "", positional.dst[index]);
	if (overrun)
		printf(" OVERRUN channel=%u need=%llu have=%llu", channel,
		       (unsigned long long)need, (unsigned long long)have);
	else
		printf(" fits");
	printf("\n");
	return 0;
}

int main(int argc, char **argv)
{
	int index;

	if (argc > 1 && !strcmp(argv[1], "--self-test")) {
		self_test();
		printf(failures ? "SELF-TEST FAILED %d\n" : "SELF-TEST OK\n",
		       failures);
		return failures ? 1 : 0;
	}

	if (argc < 2) {
		fprintf(stderr, "usage: %s --self-test | %s FILE.anec...\n",
			argv[0], argv[0]);
		return 2;
	}

	for (index = 1; index != argc; index++)
		if (report(argv[index]))
			return 1;

	return 0;
}
