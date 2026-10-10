// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

/*
 * test_t6021_prog_lookup.c — digest tests for the PROG_LOOKUP reuse.
 *
 * SHA-256 primitive: NIST FIPS 180-4 vectors, streaming equivalence in
 * odd chunks, and tail-boundary cases. The fixture digest:
 * ane_m2_program_digest matches the driver's PROG_LOAD construction,
 * is content-sensitive (one flipped byte in a section moves it), and
 * is stable for identical content.
 *
 * Both compression paths are exercised where the ARMv8 SHA-2 crypto
 * extension is compiled (arm64 + HWCAP_SHA2): the vectors and the
 * fixture digest run once on the portable path and once on the
 * hardware path, and a small benchmark prints the measured MB/s of
 * each. On hosts without the extension (the x86 CT) the hardware path
 * is not compiled and only the portable path runs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ane_sha256.h"
#include "ane_m2.h"

static int failures;

static void check(int cond, const char *what)
{
	if (!cond) {
		printf("FAIL %s\n", what);
		failures++;
	}
}

static void hexdig(const uint8_t *d, char *hex)
{
	for (int i = 0; i < ANE_SHA256_LEN; i++) {
		sprintf(hex + i * 2, "%02x", d[i]);
	}
}

/* The driver's construction, replicated from
 * ane/t6021/ane_t6021_rtclient_main.c PROG_LOAD: u64 id, u64 size,
 * then bytes, sections in caller order. */
static void ref_digest(const struct ane_m2_sections *s, uint8_t out[32])
{
	struct ane_sha256_ctx c;
	uint64_t hdr[2];
	/* The firmware section ids, hardcoded: 1,2,3,4,5,7. */
	static const uint32_t ids[ANE_M2_SEC_COUNT] = { 1, 2, 3, 4, 5, 7 };

	ane_sha256_init(&c);
	for (unsigned i = 0; i < ANE_M2_SEC_COUNT; i++) {
		hdr[0] = ids[i];
		hdr[1] = s->sec[i].size;
		ane_sha256_update(&c, hdr, sizeof(hdr));
		ane_sha256_update(&c, s->sec[i].data, s->sec[i].size);
	}
	ane_sha256_final(&c, out);
}

static void nist_all(void)
{
	uint8_t d[ANE_SHA256_LEN];
	char hex[65];

	ane_sha256("abc", 3, d);
	hexdig(d, hex);
	check(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223"
			 "b00361a396177a9cb410ff61f20015ad"),
	      "sha256(abc) NIST vector");
	ane_sha256("", 0, d);
	hexdig(d, hex);
	check(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb924"
			 "27ae41e4649b934ca495991b7852b855"),
	      "sha256(empty) NIST vector");
	ane_sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
		   56, d);
	hexdig(d, hex);
	check(!strcmp(hex, "248d6a61d20638b8e5c026930c3e6039"
			 "a33ce45964ff2167f6ecedd419db06c1"),
	      "sha256(448-bit) NIST vector");

	/* Streaming in odd chunks equals one-shot. */
	{
		uint8_t big[1000];
		struct ane_sha256_ctx c;
		uint8_t d2[ANE_SHA256_LEN];

		memset(big, 0x5a, sizeof(big));
		ane_sha256(big, sizeof(big), d);
		ane_sha256_init(&c);
		for (size_t off = 0; off < sizeof(big); off += 7) {
			size_t n = sizeof(big) - off < 7 ?
					   sizeof(big) - off : 7;
			ane_sha256_update(&c, big + off, n);
		}
		ane_sha256_final(&c, d2);
		check(!memcmp(d, d2, ANE_SHA256_LEN),
		      "streaming in 7-byte chunks equals one-shot");
	}
}

/* Build + digest the fixture; returns 0 on success. */
static int fixture_digest(const char *anec, uint8_t out[ANE_SHA256_LEN])
{
	FILE *fp = fopen(anec, "rb");
	uint8_t *buf;
	long len;
	struct ane_m2_model model;
	struct ane_m2_sections s;
	int rc = -1;

	if (!fp) {
		return -1;
	}
	fseek(fp, 0, SEEK_END);
	len = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	buf = malloc((size_t)len);
	if (fread(buf, 1, (size_t)len, fp) != (size_t)len) {
		free(buf);
		fclose(fp);
		return -1;
	}
	fclose(fp);
	if (!ane_m2_program_build(buf, (uint64_t)len, &model, &s)) {
		rc = ane_m2_program_digest(&s, out);
		ane_m2_sections_free(&s);
	}
	free(buf);
	return rc;
}

static double bench_mib_s(int portable)
{
	static uint8_t blk[64] = { 1 };
	struct ane_sha256_ctx c;
	uint8_t d[ANE_SHA256_LEN];
	const int reps = 2000000; /* 128 MiB of blocks */
	struct timespec t0, t1;
	double sec;

	(void)blk;
	ane_sha256_force_portable(portable);
	ane_sha256_init(&c);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < reps; i++) {
		blk[0] = (uint8_t)i;
		blk[1] = (uint8_t)(i >> 8);
		c.st[0] = 0x6a09e667;
		c.st[1] = 0xbb67ae85;
		c.st[2] = 0x3c6ef372;
		c.st[3] = 0xa54ff53a;
		c.st[4] = 0x510e527f;
		c.st[5] = 0x9b05688c;
		c.st[6] = 0x1f83d9ab;
		c.st[7] = 0x5be0cd19;
		c.buflen = 0;
		c.total = 0;
		ane_sha256_update(&c, blk, sizeof(blk));
		ane_sha256_final(&c, d);
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	sec = (double)(t1.tv_sec - t0.tv_sec) +
	      (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
	return (double)reps * 64.0 / 1048576.0 / sec;
}

int main(void)
{
	const char *anec = "../fixtures/h14-anec/add/program-0.anec";
	uint8_t d_portable[ANE_SHA256_LEN];
	uint8_t d[ANE_SHA256_LEN];
	int hw;

	hw = ane_sha256_hw_supported();
	printf("armv8 sha2 hw path: %s\n",
	       hw ? "compiled and supported" :
		    "not available on this host (portable path only)");

	/* Portable path (the reference): full vector + fixture suite. */
	ane_sha256_force_portable(1);
	nist_all();
	check(fixture_digest(anec, d_portable) == 0,
	      "fixture digest (portable) computed");

	/* Sensitivity: one flipped byte in a section moves the digest;
	 * identical bytes keep it stable. */
	{
		struct ane_m2_model model;
		struct ane_m2_sections s;
		uint8_t d2[ANE_SHA256_LEN];
		uint8_t d3[ANE_SHA256_LEN];
		FILE *fp = fopen(anec, "rb");
		uint8_t *buf;
		long len;

		if (!fp) {
			printf("FAIL lookup-digest: cannot read %s\n", anec);
			return 1;
		}
		fseek(fp, 0, SEEK_END);
		len = ftell(fp);
		fseek(fp, 0, SEEK_SET);
		buf = malloc((size_t)len);
		if (fread(buf, 1, (size_t)len, fp) != (size_t)len) {
			fclose(fp);
			free(buf);
			printf("FAIL lookup-digest: short read\n");
			return 1;
		}
		fclose(fp);
		check(ane_m2_program_build(buf, (uint64_t)len, &model, &s) ==
			      0,
		      "lookup-digest: fixture builds");
		ane_m2_program_digest(&s, d2);
		ref_digest(&s, d3);
		check(!memcmp(d2, d3, 32),
		      "lookup-digest: matches the driver construction");
		((uint8_t *)s.sec[1].data)[s.sec[1].size / 2] ^= 0x80;
		ane_m2_program_digest(&s, d);
		check(memcmp(d, d2, 32) != 0,
		      "lookup-digest: one flipped section byte moves it");
		((uint8_t *)s.sec[1].data)[s.sec[1].size / 2] ^= 0x80;
		ane_m2_program_digest(&s, d);
		check(memcmp(d, d2, 32) == 0,
		      "lookup-digest: identical bytes keep it stable");
		ane_m2_sections_free(&s);
		free(buf);
	}

	/* Hardware path: same vectors, same fixture digest, and a rate
	 * comparison. Only where compiled and supported. */
	if (hw) {
		uint8_t fixture_hw[ANE_SHA256_LEN];
		double p, n;

		ane_sha256_force_portable(0);
		nist_all();
		check(fixture_digest(anec, fixture_hw) == 0,
		      "fixture digest (neon) computed");
		check(!memcmp(d_portable, fixture_hw, ANE_SHA256_LEN),
		      "neon digest equals portable digest");
		p = bench_mib_s(1);
		n = bench_mib_s(0);
		printf("rate portable=%.0f MiB/s neon=%.0f MiB/s\n", p, n);
	} else {
		printf("hw path not exercised on this host\n");
	}
	ane_sha256_force_portable(0);

	printf(failures ? "LOOKUP-DIGEST FAIL\n" : "LOOKUP-DIGEST PASS\n");
	return failures != 0;
}
