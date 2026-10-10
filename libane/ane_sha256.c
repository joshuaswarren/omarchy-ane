// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */
/*
 * ane_sha256.c — compact FIPS 180-4 SHA-256 for the M2 program-digest
 * (the same key the driver's PROG_LOAD dedup computes over every
 * section's id, size and bytes). Incremental.
 *
 * Two compression paths: a portable C block and, on arm64, the ARMv8
 * SHA-2 crypto extension (vsha256hq/h2q/su0/su1) selected at runtime
 * via getauxval(AT_HWCAP) & HWCAP_SHA2 -- per-function target
 * attribute, so the rest of libane still builds for the baseline.
 * The portable path is the fallback and the reference; the NIST tests
 * in tools/test_t6021_prog_lookup.c run against both where the
 * hardware path is compiled.
 */
#include "ane_sha256.h"

#include <string.h>
#if defined(__aarch64__)
#include <sys/auxv.h>
#include <elf.h>
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
#include <arm_neon.h>

/* ARMv8 SHA-2 crypto extension: one 64-byte block per call with the
 * vsha256hq/h2q round groups and vsha256su0/su1 schedule updates. The
 * canonical lane convention: the FIPS state is loaded/stored in
 * natural word order (st[0..3] = ABCD, st[4..7] = EFGH) and the
 * message quadwords are consumed in byte-stream order. Kept behind a
 * per-function target attribute so the rest of libane stays
 * baseline. */
__attribute__((target("crypto")))
static void sha256_block_neon(uint32_t st[8], const uint8_t p[64])
{
	uint32x4_t STATE0, STATE1, MSG, TMP;
	uint32x4_t MSG0, MSG1, MSG2, MSG3;
	uint32x4_t ABEF_SAVE, CDGH_SAVE;

	STATE0 = vld1q_u32(&st[0]);
	STATE1 = vld1q_u32(&st[4]);

	ABEF_SAVE = STATE0;
	CDGH_SAVE = STATE1;

	/* Rounds 0-3 */
	MSG0 = vld1q_u32((const uint32_t *)(p + 0));
	MSG = vaddq_u32(MSG0, vld1q_u32(&K[0]));
	STATE1 = vsha256hq_u32(STATE1, STATE0, MSG);
	STATE0 = vsha256h2q_u32(STATE0, STATE1, MSG);

	/* Rounds 4-7 */
	MSG1 = vld1q_u32((const uint32_t *)(p + 16));
	MSG = vaddq_u32(MSG1, vld1q_u32(&K[4]));
	STATE1 = vsha256hq_u32(STATE1, STATE0, MSG);
	STATE0 = vsha256h2q_u32(STATE0, STATE1, MSG);

	/* Rounds 8-11 */
	MSG2 = vld1q_u32((const uint32_t *)(p + 32));
	MSG = vaddq_u32(MSG2, vld1q_u32(&K[8]));
	STATE1 = vsha256hq_u32(STATE1, STATE0, MSG);
	STATE0 = vsha256h2q_u32(STATE0, STATE1, MSG);

	/* Rounds 12-15 */
	MSG3 = vld1q_u32((const uint32_t *)(p + 48));
	MSG = vaddq_u32(MSG3, vld1q_u32(&K[12]));
	STATE1 = vsha256hq_u32(STATE1, STATE0, MSG);
	STATE0 = vsha256h2q_u32(STATE0, STATE1, MSG);

	/* Rounds 16-19 */
	MSG0 = vsha256su0q_u32(MSG0, MSG1);
	MSG = vaddq_u32(MSG0, vld1q_u32(&K[16]));
	TMP = vsha256su1q_u32(MSG0, MSG3);
	MSG1 = vsha256su1q_u32(MSG1, TMP);
	STATE1 = vsha256hq_u32(STATE1, STATE0, MSG);
	STATE0 = vsha256h2q_u32(STATE0, STATE1, MSG);

	/* Rounds 20-23 */
	MSG1 = vsha256su0q_u32(MSG1, MSG2);
	MSG = vaddq_u32(MSG1, vld1q_u32(&K[20]));
	TMP = vsha256su1q_u32(MSG1, MSG0);
	MSG2 = vsha256su1q_u32(MSG2, TMP);
	STATE1 = vsha256hq_u32(STATE1, STATE0, MSG);
	STATE0 = vsha256h2q_u32(STATE0, STATE1, MSG);

	/* Rounds 24-27 */
	MSG2 = vsha256su0q_u32(MSG2, MSG3);
	MSG = vaddq_u32(MSG2, vld1q_u32(&K[24]));
	TMP = vsha256su1q_u32(MSG2, MSG1);
	MSG3 = vsha256su1q_u32(MSG3, TMP);
	STATE1 = vsha256hq_u32(STATE1, STATE0, MSG);
	STATE0 = vsha256h2q_u32(STATE0, STATE1, MSG);

	/* Rounds 28-31 */
	MSG3 = vsha256su0q_u32(MSG3, MSG0);
	MSG = vaddq_u32(MSG3, vld1q_u32(&K[28]));
	TMP = vsha256su1q_u32(MSG3, MSG2);
	MSG0 = vsha256su1q_u32(MSG0, TMP);
	STATE1 = vsha256hq_u32(STATE1, STATE0, MSG);
	STATE0 = vsha256h2q_u32(STATE0, STATE1, MSG);

	/* Rounds 32-35 */
	MSG0 = vsha256su0q_u32(MSG0, MSG1);
	MSG = vaddq_u32(MSG0, vld1q_u32(&K[32]));
	TMP = vsha256su1q_u32(MSG0, MSG3);
	MSG1 = vsha256su1q_u32(MSG1, TMP);
	STATE1 = vsha256hq_u32(STATE1, STATE0, MSG);
	STATE0 = vsha256h2q_u32(STATE0, STATE1, MSG);

	/* Rounds 36-39 */
	MSG1 = vsha256su0q_u32(MSG1, MSG2);
	MSG = vaddq_u32(MSG1, vld1q_u32(&K[36]));
	TMP = vsha256su1q_u32(MSG1, MSG0);
	MSG2 = vsha256su1q_u32(MSG2, TMP);
	STATE1 = vsha256hq_u32(STATE1, STATE0, MSG);
	STATE0 = vsha256h2q_u32(STATE0, STATE1, MSG);

	/* Rounds 40-43 */
	MSG2 = vsha256su0q_u32(MSG2, MSG3);
	MSG = vaddq_u32(MSG2, vld1q_u32(&K[40]));
	TMP = vsha256su1q_u32(MSG2, MSG1);
	MSG3 = vsha256su1q_u32(MSG3, TMP);
	STATE1 = vsha256hq_u32(STATE1, STATE0, MSG);
	STATE0 = vsha256h2q_u32(STATE0, STATE1, MSG);

	/* Rounds 44-47 */
	MSG3 = vsha256su0q_u32(MSG3, MSG0);
	MSG = vaddq_u32(MSG3, vld1q_u32(&K[44]));
	TMP = vsha256su1q_u32(MSG3, MSG2);
	MSG0 = vsha256su1q_u32(MSG0, TMP);
	STATE1 = vsha256hq_u32(STATE1, STATE0, MSG);
	STATE0 = vsha256h2q_u32(STATE0, STATE1, MSG);

	/* Rounds 48-51 */
	MSG0 = vsha256su0q_u32(MSG0, MSG1);
	MSG = vaddq_u32(MSG0, vld1q_u32(&K[48]));
	TMP = vsha256su1q_u32(MSG0, MSG3);
	MSG1 = vsha256su1q_u32(MSG1, TMP);
	STATE1 = vsha256hq_u32(STATE1, STATE0, MSG);
	STATE0 = vsha256h2q_u32(STATE0, STATE1, MSG);

	/* Rounds 52-55 */
	MSG1 = vsha256su0q_u32(MSG1, MSG2);
	MSG = vaddq_u32(MSG1, vld1q_u32(&K[52]));
	TMP = vsha256su1q_u32(MSG1, MSG0);
	MSG2 = vsha256su1q_u32(MSG2, TMP);
	STATE1 = vsha256hq_u32(STATE1, STATE0, MSG);
	STATE0 = vsha256h2q_u32(STATE0, STATE1, MSG);

	/* Rounds 56-59 */
	MSG2 = vsha256su0q_u32(MSG2, MSG3);
	MSG = vaddq_u32(MSG2, vld1q_u32(&K[56]));
	TMP = vsha256su1q_u32(MSG2, MSG1);
	MSG3 = vsha256su1q_u32(MSG3, TMP);
	STATE1 = vsha256hq_u32(STATE1, STATE0, MSG);
	STATE0 = vsha256h2q_u32(STATE0, STATE1, MSG);

	/* Rounds 60-63 */
	MSG3 = vsha256su0q_u32(MSG3, MSG0);
	MSG = vaddq_u32(MSG3, vld1q_u32(&K[60]));
	STATE1 = vsha256hq_u32(STATE1, STATE0, MSG);
	STATE0 = vsha256h2q_u32(STATE0, STATE1, MSG);

	STATE0 = vaddq_u32(STATE0, ABEF_SAVE);
	STATE1 = vaddq_u32(STATE1, CDGH_SAVE);
	vst1q_u32(&st[0], STATE0);
	vst1q_u32(&st[4], STATE1);
}
#endif /* __aarch64__ */

static void (*sha256_block_fn)(uint32_t st[8], const uint8_t p[64]) =
	sha256_block_portable;
static int sha256_portable_forced;
#if defined(__aarch64__)
static int sha256_neon_ok = -1;
#endif

static void choose_block(void)
{
#if defined(__aarch64__)
	if (sha256_neon_ok < 0) {
		sha256_neon_ok =
			(getauxval(AT_HWCAP) & HWCAP_SHA2) != 0;
	}
	if (!sha256_portable_forced && sha256_neon_ok) {
		sha256_block_fn = sha256_block_neon;
		return;
	}
#endif
	sha256_block_fn = sha256_block_portable;
}

int ane_sha256_hw_supported(void)
{
#if defined(__aarch64__)
	return sha256_neon_ok >= 0 ? sha256_neon_ok :
				     (getauxval(AT_HWCAP) & HWCAP_SHA2) != 0;
#else
	return 0;
#endif
}

void ane_sha256_force_portable(int force)
{
	sha256_portable_forced = force;
}

void ane_sha256_init(struct ane_sha256_ctx *c)
{
	choose_block();
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
		sha256_block_fn(c->st, c->buf);
		c->buflen = 0;
		p += take;
		len -= take;
	}
	while (len >= 64) {
		sha256_block_fn(c->st, p);
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
		sha256_block_fn(c->st, tail);
		memset(tail, 0, sizeof(tail));
	}
	for (i = 0; i < 8; i++) {
		tail[63 - i] = (uint8_t)(bits >> (8 * i));
	}
	sha256_block_fn(c->st, tail);
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
