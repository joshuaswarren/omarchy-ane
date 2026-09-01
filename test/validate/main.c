// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren <816217+joshuaswarren@users.noreply.github.com> */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../ane_utils.h"
#include "ane.h"

// clang-format off

static int tests_run;
static int tests_failed;

#define CHECK(cond)                                                    \
	do {                                                           \
		tests_run++;                                           \
		if (!(cond)) {                                         \
			tests_failed++;                                \
			ane_err("FAIL %d: %s\n", __LINE__, #cond);     \
		}                                                      \
	} while (0)

/* anec members are const-qualified; the library loads them by block
 * copy (ane_model_init), so tests configure through the same bytes */
static void set_anec(struct ane_nn *nn, uint64_t tsk_size,
		     uint32_t src_count, uint32_t dst_count,
		     uint32_t cmd_tiles, uint32_t src_tiles,
		     uint32_t dst_tiles)
{
	uint8_t raw[0x6a8];

	memset(raw, 0, sizeof(raw));
	memcpy(raw + offsetof(struct anec, tsk_size), &tsk_size,
	       sizeof(tsk_size));
	memcpy(raw + offsetof(struct anec, src_count), &src_count,
	       sizeof(src_count));
	memcpy(raw + offsetof(struct anec, dst_count), &dst_count,
	       sizeof(dst_count));
	memcpy(raw + offsetof(struct anec, tiles[0]), &cmd_tiles,
	       sizeof(cmd_tiles));
	memcpy(raw + offsetof(struct anec, tiles[4]), &dst_tiles,
	       sizeof(dst_tiles));
	memcpy(raw + offsetof(struct anec, tiles[4 + dst_count]), &src_tiles,
	       sizeof(src_tiles));
	memcpy(&nn->anec, raw, sizeof(raw));
}

/* a fake nn: no device, chans[0] backed by a real tile-sized buffer */
static struct ane_nn *make_nn(uint64_t cmd_size)
{
	struct ane_nn *nn = ane_zmalloc(sizeof(*nn));
	if (nn == NULL) {
		return NULL;
	}

	nn->fd = -1;
	nn->chans[0].size = cmd_size;
	nn->chans[0].map = ane_zmalloc(cmd_size);
	if (nn->chans[0].map == NULL) {
		free(nn);
		return NULL;
	}

	return nn;
}

static void free_nn(struct ane_nn *nn)
{
	free(nn->chans[0].map);
	free(nn);
}

static void test_abi_layout(void)
{
	struct ane_nn *nn = make_nn(0x4000);

	CHECK(nn != NULL);
	CHECK(TILE_COUNT == 0x20);
	/* 8 + 4 + 4 + 8 + 8 + 4 + 4 + 32*4 + 32*6*8 */
	CHECK(sizeof(struct anec) == 0x6a8);
	CHECK(sizeof(struct ane_bo) == 32);
	CHECK(sizeof(((struct anec *)0)->tiles) == 0x20 * 4);
	CHECK(sizeof(((struct anec *)0)->nchw) == 0x20 * 6 * 8);

	free_nn(nn);
}

static void test_kernel_capacity(void)
{
	struct ane_nn *nn = make_nn(0x4000);

	CHECK(nn != NULL);
	set_anec(nn, 0x574, 0, 0, 1, 0, 0);
	/* round_up(0x574, 16) = 0x580 */
	CHECK(ane_kernel_capacity(nn) == 0x4000 - 0x580);

	set_anec(nn, 0x575, 0, 0, 1, 0, 0); /* unaligned rounds up the same */
	CHECK(ane_kernel_capacity(nn) == 0x4000 - 0x580);

	nn->chans[0].size = 0x580; /* task region fills the buffer */
	CHECK(ane_kernel_capacity(nn) == 0);

	nn->chans[0].size = 0x400; /* task region overflows */
	CHECK(ane_kernel_capacity(nn) == 0);

	free_nn(nn);
}

static void test_bind_kernel(void)
{
	struct ane_nn *nn = make_nn(0x4000);
	uint8_t payload[0x100];
	uint8_t payload2[0x100];
	uint64_t capacity;
	uint8_t *big;

	CHECK(nn != NULL);
	set_anec(nn, 0x574, 0, 0, 1, 0, 0);

	for (int i = 0; i < 0x100; i++) {
		payload[i] = 0xa0 | (i & 0xf);
		payload2[i] = 0xb0 | (i & 0xf);
	}

	capacity = ane_kernel_capacity(nn);
	CHECK(capacity == 0x3a80);

	CHECK(ane_bind_kernel(nn, NULL, 0x100) == -EINVAL);
	CHECK(ane_bind_kernel(nn, payload, capacity + 1) == -EINVAL);
	CHECK(ane_bind_kernel(nn, payload, 0) == 0);

	/* payload lands at round_up(tsk_size, 16), nothing else moves */
	CHECK(ane_bind_kernel(nn, payload, 0x100) == 0);
	CHECK(((uint8_t *)nn->chans[0].map)[0x57f] == 0);
	CHECK(memcmp((uint8_t *)nn->chans[0].map + 0x580, payload, 0x100) ==
	      0);
	CHECK(((uint8_t *)nn->chans[0].map)[0x680] == 0);

	/* rebind overwrites in place */
	CHECK(ane_bind_kernel(nn, payload2, 0x100) == 0);
	CHECK(memcmp((uint8_t *)nn->chans[0].map + 0x580, payload2, 0x100) ==
	      0);

	/* exact-capacity bind is legal and spans to the end */
	big = ane_malloc(capacity);
	CHECK(big != NULL);
	memset(big, 0xc5, capacity);
	memset(nn->chans[0].map, 0, nn->chans[0].size);
	CHECK(ane_bind_kernel(nn, big, capacity) == 0);
	CHECK(((uint8_t *)nn->chans[0].map)[0x4000 - 1] == 0xc5);
	free(big);

	free_nn(nn);
}

static void test_exec_loop_rejects(void)
{
	struct ane_nn *nn = make_nn(0x4000);

	CHECK(nn != NULL);
	set_anec(nn, 0x574, 1, 1, 1, 0, 0);

	CHECK(ane_exec_loop(nn, 0, 0, 0) == -EINVAL);
	CHECK(ane_exec_loop(nn, 3, 1, 0) == -EINVAL);
	CHECK(ane_exec_loop(nn, 3, 0, 1) == -EINVAL);

	/* state in/out sizes differ: 0x4000 src vs 0x8000 dst */
	set_anec(nn, 0x574, 1, 1, 1, 1, 2);
	CHECK(ane_exec_loop(nn, 3, 0, 0) == -EINVAL);

	free_nn(nn);
}

int main(void)
{
	test_abi_layout();
	test_kernel_capacity();
	test_bind_kernel();
	test_exec_loop_rejects();

	printf("TEST: %d checks, %d failed\n", tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}

// clang-format on
