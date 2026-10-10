// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */
/*
 * ane_sha256.c — compact FIPS 180-4 SHA-256 for the M2 program-digest
 * (the same key the driver's PROG_LOAD dedup computes over every
 * section's id, size and bytes). Incremental.
 *
 * Two compression paths: a portable C block (the reference) and, on
 * arm64, the ARMv8 SHA-2 crypto extension (vsha256hq/h2q/su0/su1).
 * The choice is PER CONTEXT (chosen once at ane_sha256_init from a
 * one-time HWCAP probe), never a global, so concurrent contexts
 * cannot race on it; ane_sha256_ctx_force_portable() is a per-context
 * override for tests. The portable path is the fallback and the
 * reference; the NIST tests in tools/test_t6021_prog_lookup.c run
 * against both paths where the hardware path is compiled, and
 * tools/Makefile cross-checks this file with the aarch64 compiler
 * where one exists.
 */
#include "ane_sha256.h"

#include <string.h>
#if defined(__aarch64__)
#include <sys/auxv.h>
#include <elf.h>
#include <arm_neon.h>
#endif

static const uint32_t K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
	0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
	0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
	0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
	0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
	0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
	0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
	0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
	0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
	0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
	0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static uint32_t rotr(uint32_t x, unsigned n)
{
	return (x >> n) | (x << (32 - n));
}

/* The reference: plain C, every platform. */
static void sha256_block_portable(uint32_t st[8], const uint8_t p[64])
{
	uint32_t w[64];
	uint32_t a, b, c, d, e, f, g, h;
	unsigned i;

	for (i = 0; i < 16; i++) {
		w[i] = (uint32_t)p[i * 4] << 24 |
		       (uint32_t)p[i * 4 + 1] << 16 |
		       (uint32_t)p[i * 4 + 2] << 8 |
		       (uint32_t)p[i * 4 + 3];
	}
	for (i = 16; i < 64; i++) {
		uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^
			      (w[i - 15] >> 3);
		uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^
			      (w[i - 2] >> 10);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}
	a = st[0];
	b = st[1];
	c = st[2];
	d = st[3];
	e = st[4];
	f = st[5];
	g = st[6];
	h = st[7];
	for (i = 0; i < 64; i++) {
		uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
		uint32_t ch = (e & f) ^ (~e & g);
		uint32_t t1 = h + S1 + ch + K[i] + w[i];
		uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t t2 = S0 + maj;

		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = c;
		c = b;
		b = a;
		a = t1 + t2;
	}
	st[0] += a;
	st[1] += b;
	st[2] += c;
	st[3] += d;
	st[4] += e;
	st[5] += f;
	st[6] += g;
	st[7] += h;
}

#if defined(__aarch64__)
/*
 * The ARMv8 SHA-2 crypto extension: one 64-byte block per call.
 * Vendored from the canonical public-domain implementation (Jeffrey
 * Walton, SHA-Intrinsics sha256-arm.c; based on ARM's own example),
 * keeping its exact hq/h2q pattern and interleaved schedule updates:
 * a wrong lane order or schedule grouping shows up immediately as a
 * NIST mismatch in tools/test_t6021_prog_lookup.c.
 */
__attribute__((target("arch=armv8-a+crypto")))
static void sha256_block_neon(uint32_t st[8], const uint8_t data[64])
{
	uint32x4_t STATE0, STATE1, ABEF_SAVE, CDGH_SAVE;
	uint32x4_t MSG0, MSG1, MSG2, MSG3;
	uint32x4_t TMP0, TMP1, TMP2;

	/* Load state */
	STATE0 = vld1q_u32(&st[0]);
	STATE1 = vld1q_u32(&st[4]);

	/* Save state */
	ABEF_SAVE = STATE0;
	CDGH_SAVE = STATE1;

	/* Load message */
	MSG0 = vld1q_u32((const uint32_t *)(data + 0));
	MSG1 = vld1q_u32((const uint32_t *)(data + 16));
	MSG2 = vld1q_u32((const uint32_t *)(data + 32));
	MSG3 = vld1q_u32((const uint32_t *)(data + 48));

	/* Reverse for little endian */
	MSG0 = vreinterpretq_u32_u8(vrev32q_u8(vreinterpretq_u8_u32(MSG0)));
	MSG1 = vreinterpretq_u32_u8(vrev32q_u8(vreinterpretq_u8_u32(MSG1)));
	MSG2 = vreinterpretq_u32_u8(vrev32q_u8(vreinterpretq_u8_u32(MSG2)));
	MSG3 = vreinterpretq_u32_u8(vrev32q_u8(vreinterpretq_u8_u32(MSG3)));

	TMP0 = vaddq_u32(MSG0, vld1q_u32(&K[0x00]));

	/* Rounds 0-3 */
	MSG0 = vsha256su0q_u32(MSG0, MSG1);
	TMP2 = STATE0;
	TMP1 = vaddq_u32(MSG1, vld1q_u32(&K[0x04]));
	STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
	STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
	MSG0 = vsha256su1q_u32(MSG0, MSG2, MSG3);

	/* Rounds 4-7 */
	MSG1 = vsha256su0q_u32(MSG1, MSG2);
	TMP2 = STATE0;
	TMP0 = vaddq_u32(MSG2, vld1q_u32(&K[0x08]));
	STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
	STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
	MSG1 = vsha256su1q_u32(MSG1, MSG3, MSG0);

	/* Rounds 8-11 */
	MSG2 = vsha256su0q_u32(MSG2, MSG3);
	TMP2 = STATE0;
	TMP1 = vaddq_u32(MSG3, vld1q_u32(&K[0x0c]));
	STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
	STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
	MSG2 = vsha256su1q_u32(MSG2, MSG0, MSG1);

	/* Rounds 12-15 */
	MSG3 = vsha256su0q_u32(MSG3, MSG0);
	TMP2 = STATE0;
	TMP0 = vaddq_u32(MSG0, vld1q_u32(&K[0x10]));
	STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
	STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
	MSG3 = vsha256su1q_u32(MSG3, MSG1, MSG2);

	/* Rounds 16-19 */
	MSG0 = vsha256su0q_u32(MSG0, MSG1);
	TMP2 = STATE0;
	TMP1 = vaddq_u32(MSG1, vld1q_u32(&K[0x14]));
	STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
	STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
	MSG0 = vsha256su1q_u32(MSG0, MSG2, MSG3);

	/* Rounds 20-23 */
	MSG1 = vsha256su0q_u32(MSG1, MSG2);
	TMP2 = STATE0;
	TMP0 = vaddq_u32(MSG2, vld1q_u32(&K[0x18]));
	STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
	STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
	MSG1 = vsha256su1q_u32(MSG1, MSG3, MSG0);

	/* Rounds 24-27 */
	MSG2 = vsha256su0q_u32(MSG2, MSG3);
	TMP2 = STATE0;
	TMP1 = vaddq_u32(MSG3, vld1q_u32(&K[0x1c]));
	STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
	STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
	MSG2 = vsha256su1q_u32(MSG2, MSG0, MSG1);

	/* Rounds 28-31 */
	MSG3 = vsha256su0q_u32(MSG3, MSG0);
	TMP2 = STATE0;
	TMP0 = vaddq_u32(MSG0, vld1q_u32(&K[0x20]));
	STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
	STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
	MSG3 = vsha256su1q_u32(MSG3, MSG1, MSG2);

	/* Rounds 32-35 */
	MSG0 = vsha256su0q_u32(MSG0, MSG1);
	TMP2 = STATE0;
	TMP1 = vaddq_u32(MSG1, vld1q_u32(&K[0x24]));
	STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
	STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
	MSG0 = vsha256su1q_u32(MSG0, MSG2, MSG3);

	/* Rounds 36-39 */
	MSG1 = vsha256su0q_u32(MSG1, MSG2);
	TMP2 = STATE0;
	TMP0 = vaddq_u32(MSG2, vld1q_u32(&K[0x28]));
	STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
	STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
	MSG1 = vsha256su1q_u32(MSG1, MSG3, MSG0);

	/* Rounds 40-43 */
	MSG2 = vsha256su0q_u32(MSG2, MSG3);
	TMP2 = STATE0;
	TMP1 = vaddq_u32(MSG3, vld1q_u32(&K[0x2c]));
	STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
	STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);
	MSG2 = vsha256su1q_u32(MSG2, MSG0, MSG1);

	/* Rounds 44-47 */
	MSG3 = vsha256su0q_u32(MSG3, MSG0);
	TMP2 = STATE0;
	TMP0 = vaddq_u32(MSG0, vld1q_u32(&K[0x30]));
	STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
	STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);
	MSG3 = vsha256su1q_u32(MSG3, MSG1, MSG2);

	/* Rounds 48-51 */
	TMP2 = STATE0;
	TMP1 = vaddq_u32(MSG1, vld1q_u32(&K[0x34]));
	STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
	STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);

	/* Rounds 52-55 */
	TMP2 = STATE0;
	TMP0 = vaddq_u32(MSG2, vld1q_u32(&K[0x38]));
	STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
	STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);

	/* Rounds 56-59 */
	TMP2 = STATE0;
	TMP1 = vaddq_u32(MSG3, vld1q_u32(&K[0x3c]));
	STATE0 = vsha256hq_u32(STATE0, STATE1, TMP0);
	STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP0);

	/* Rounds 60-63 */
	TMP2 = STATE0;
	STATE0 = vsha256hq_u32(STATE0, STATE1, TMP1);
	STATE1 = vsha256h2q_u32(STATE1, TMP2, TMP1);

	/* Combine state */
	STATE0 = vaddq_u32(STATE0, ABEF_SAVE);
	STATE1 = vaddq_u32(STATE1, CDGH_SAVE);

	/* Save state */
	vst1q_u32(&st[0], STATE0);
	vst1q_u32(&st[4], STATE1);
}
#endif /* __aarch64__ */

int ane_sha256_hw_supported(void)
{
#if defined(__aarch64__)
	return (getauxval(AT_HWCAP) & HWCAP_SHA2) != 0;
#else
	return 0;
#endif
}

void ane_sha256_init(struct ane_sha256_ctx *c)
{
	/* Per-context choice, no mutable global: getauxval only reads the
	 * immutable auxiliary vector, so concurrent first inits cannot race
	 * (w7K review of 69731a8, finding 2). */
	c->neon = ane_sha256_hw_supported();
	c->portable_forced = 0;
	c->st[0] = 0x6a09e667;
	c->st[1] = 0xbb67ae85;
	c->st[2] = 0x3c6ef372;
	c->st[3] = 0xa54ff53a;
	c->st[4] = 0x510e527f;
	c->st[5] = 0x9b05688c;
	c->st[6] = 0x1f83d9ab;
	c->st[7] = 0x5be0cd19;
	c->buflen = 0;
	c->total = 0;
}

void ane_sha256_ctx_force_portable(struct ane_sha256_ctx *c, int force)
{
	c->portable_forced = force ? 1 : 0;
}

static void sha256_block(struct ane_sha256_ctx *c, const uint8_t p[64])
{
#if defined(__aarch64__)
	if (c->neon && !c->portable_forced) {
		sha256_block_neon(c->st, p);
		return;
	}
#endif
	sha256_block_portable(c->st, p);
}

void ane_sha256_update(struct ane_sha256_ctx *c, const void *data,
		       uint64_t len)
{
	const uint8_t *p = data;

	c->total += len;
	if (c->buflen) {
		uint64_t take = 64 - c->buflen;

		if (take > len) {
			memcpy(c->buf + c->buflen, p, (size_t)len);
			c->buflen += (unsigned)len;
			return;
		}
		memcpy(c->buf + c->buflen, p, (size_t)take);
		sha256_block(c, c->buf);
		c->buflen = 0;
		p += take;
		len -= take;
	}
	while (len >= 64) {
		sha256_block(c, p);
		p += 64;
		len -= 64;
	}
	memcpy(c->buf, p, (size_t)len);
	c->buflen = (unsigned)len;
}

void ane_sha256_final(struct ane_sha256_ctx *c, uint8_t out[ANE_SHA256_LEN])
{
	uint8_t tail[64] = { 0 };
	uint64_t bits = c->total << 3;
	unsigned rem = c->buflen;
	unsigned i;

	memcpy(tail, c->buf, rem);
	tail[rem] = 0x80;
	if (rem >= 56) {
		sha256_block(c, tail);
		memset(tail, 0, sizeof(tail));
	}
	for (i = 0; i < 8; i++) {
		tail[63 - i] = (uint8_t)(bits >> (8 * i));
	}
	sha256_block(c, tail);
	for (i = 0; i < 8; i++) {
		out[i * 4] = (uint8_t)(c->st[i] >> 24);
		out[i * 4 + 1] = (uint8_t)(c->st[i] >> 16);
		out[i * 4 + 2] = (uint8_t)(c->st[i] >> 8);
		out[i * 4 + 3] = (uint8_t)c->st[i];
	}
}

void ane_sha256(const void *data, uint64_t len, uint8_t out[ANE_SHA256_LEN])
{
	struct ane_sha256_ctx c;

	ane_sha256_init(&c);
	ane_sha256_update(&c, data, len);
	ane_sha256_final(&c, out);
}
