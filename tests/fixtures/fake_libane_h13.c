/* SPDX-License-Identifier: MIT */
/* Copyright 2026 Joshua Warren */

/* Fake libane for host tests of the H13 ABI-1 tools (ane-session-h13,
 * ane-cycle). No device is opened: two inputs and one output of
 * FAKE_CH bytes each. The fixture program (fixtures/h13-anec/add) is the
 * H13 add, so exec reproduces its semantics: fp16 lanes at stride 64,
 * fp16 add with ties away from zero, untouched bytes zero. This matches
 * the driver's model_chain_plane elementwise (fp64 add + round-half-away
 * re-encode of the same fp16 values), so the CALL path exercises the full
 * send/exec/read round trip against an independent oracle. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ane.h"

#define FAKE_CH 16384
#define FAKE_PLANE_STRIDE 64
#define FAKE_IN 2
#define FAKE_OUT 1

static struct ane_nn fake_nn = {
	.anec = { .src_count = FAKE_IN, .dst_count = FAKE_OUT },
};
static uint8_t fake_in[FAKE_IN][FAKE_CH];
static uint8_t fake_out[FAKE_CH];

struct ane_nn *__ane_init(const char *path, int dev_id)
{
	FILE *f = fopen(path, "rb");

	(void)dev_id;
	if (!f)
		return NULL;
	fclose(f);
	return &fake_nn;
}

void __ane_free(struct ane_nn *nn)
{
	(void)nn;
}

uint64_t __ane_src_size(struct ane_nn *nn, const uint32_t idx)
{
	(void)nn;
	(void)idx;
	return FAKE_CH;
}

uint64_t __ane_dst_size(struct ane_nn *nn, const uint32_t idx)
{
	(void)nn;
	(void)idx;
	return FAKE_CH;
}

void __ane_send(struct ane_nn *nn, void *from, const uint32_t idx)
{
	(void)nn;
	if (idx < FAKE_IN)
		memcpy(fake_in[idx], from, FAKE_CH);
}

void __ane_read(struct ane_nn *nn, void *to, const uint32_t idx)
{
	(void)nn;
	if (idx < FAKE_OUT)
		memcpy(to, fake_out, FAKE_CH);
}

/* fp16 value in exact 2^-24 units; subnormals count in frac units. */
static int fp16_units(uint16_t bits)
{
	int sign = bits >> 15;
	int exp = (bits >> 10) & 31;
	int frac = bits & 1023;
	int value = exp == 0 ? frac : (1024 + frac) << (exp - 1);

	return sign ? -value : value;
}

static int bit_length(int x)
{
	int b = 1;

	while (x >>= 1)
		b++;
	return b;
}

/* Round to nearest, ties away from zero; re-encode as fp16 bits. */
static uint16_t round_fp16_units(int value)
{
	uint16_t sign = value < 0 ? 0x8000 : 0;
	int n = value < 0 ? -value : value;
	int shift, quantum, exponent, significand;

	if (n < 1024)
		return sign | (uint16_t)n;
	shift = n - 11 > 0 ? bit_length(n) - 11 : 0;
	quantum = 1 << shift;
	n = ((n + quantum / 2) / quantum) * quantum;
	exponent = bit_length(n) - 10;
	significand = n >> (exponent - 1);
	if (significand == 2048) {
		exponent += 1;
		significand = 1024;
	}
	return sign | (uint16_t)(exponent << 10) |
	       (uint16_t)(significand - 1024);
}

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)(v >> 8);
}

int ane_exec(struct ane_nn *nn)
{
	uint32_t off;

	(void)nn;
	memset(fake_out, 0, FAKE_CH);
	for (off = 0; off + 2 <= FAKE_CH; off += FAKE_PLANE_STRIDE) {
		int sum = fp16_units(le16(fake_in[0] + off)) +
			  fp16_units(le16(fake_in[1] + off));

		put_le16(fake_out + off, round_fp16_units(sum));
	}
	return 0;
}
