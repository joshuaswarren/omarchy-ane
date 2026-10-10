// SPDX-License-Identifier: MIT
/* Copyright 2026 Joshua Warren */

/*
// test_t6021_prog_lookup.c — host digest tests for the PROG_LOOKUP
// reuse: the SHA-256 primitive (NIST vectors, streaming equivalence)
// and ane_m2_program_digest's construction (the driver's PROG_LOAD
// key: u64 id, u64 size, bytes, sections in id order). A one-byte
// change must move the digest; the same bytes must not.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

	ane_sha256_init(&c);
	for (unsigned i = 0; i < ANE_M2_SEC_COUNT; i++) {
		/* The firmware section ids, hardcoded: 1,2,3,4,5,7. */
		static const uint32_t ids[ANE_M2_SEC_COUNT] = {
			1, 2, 3, 4, 5, 7
		};

		hdr[0] = ids[i];
		hdr[1] = s->sec[i].size;
		ane_sha256_update(&c, hdr, sizeof(hdr));
		ane_sha256_update(&c, s->sec[i].data, s->sec[i].size);
	}
	ane_sha256_final(&c, out);
}

int main(void)
{
	uint8_t d[ANE_SHA256_LEN], d2[ANE_SHA256_LEN];
	char hex[65];

	/* NIST FIPS 180-4 vectors. */
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

	/* ane_m2_program_digest: matches the driver construction, is
	 * content-sensitive, and ignores nothing. */
	{
		struct ane_m2_sections s;
		uint8_t base[ANE_SHA256_LEN];
		struct ane_m2_model model;
		const char anec[] = "../fixtures/h14-anec/add/program-0.anec";
		FILE *fp = fopen(anec, "rb");
		uint8_t *buf;
		long len;
		int rc;

		if (!fp) {
			printf("FAIL lookup-digest: cannot read %s\n", anec);
			return 1;
		}
		fseek(fp, 0, SEEK_END);
		len = ftell(fp);
		fseek(fp, 0, SEEK_SET);
		buf = malloc((size_t)len);
		if (fread(buf, 1, (size_t)len, fp) != (size_t)len) {
			printf("FAIL lookup-digest: short read\n");
			failures++;
			fclose(fp);
			free(buf);
			return 1;
		}
		fclose(fp);
		rc = ane_m2_program_build(buf, (uint64_t)len, &model, &s);
		check(rc == 0, "lookup-digest: fixture builds");
		if (rc != 0) {
			free(buf);
			return 1;
		}
		check(ane_m2_program_digest(&s, base) == 0,
		      "lookup-digest: program digest computed");
		ref_digest(&s, d2);
		check(!memcmp(base, d2, 32),
		      "lookup-digest: matches the driver construction");

		/* One flipped byte in a section: digest MUST move. */
		((uint8_t *)s.sec[1].data)[s.sec[1].size / 2] ^= 0x80;
		ane_m2_program_digest(&s, d);
		check(memcmp(d, base, 32) != 0,
		      "lookup-digest: one flipped section byte moves it");

		ane_m2_sections_free(&s);
		free(buf);
	}

	printf(failures ? "LOOKUP-DIGEST FAIL\n" : "LOOKUP-DIGEST PASS\n");
	return failures != 0;
}
