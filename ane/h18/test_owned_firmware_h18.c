// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Host unit caller of the real production entry. Reads the exact public
 * T8152 ANE0 payload, stages it through ane_h18_firmware_stage, and checks
 * byte-exact TEXT/DATA placement, the entire zero tail, input independence,
 * exact-pin refusals, busy-owner and failure-preserving semantics, and
 * idempotent release. Storage semantics only: no device, DMA, boot or
 * firmware execution is implied. */
#include <errno.h>
#include <err.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ane_h18_firmware.h"
#include "ane_h18_fw_pins.h"
#include "ane_h18_fw_layout.h"
#include "ane_sha256.h"

#define PAYLOAD_SIZE ((uint64_t)1572864u)

_Static_assert(ANE_H18_FW_TEXT_SIZE + ANE_H18_FW_DATA_FILE_SIZE ==
	       ANE_H18_FW_TEXT_SIZE + 0xb8000u, "data span");
_Static_assert(ANE_H18_FW_IMAGE_SIZE == 0x370000u, "footprint");

/* Other-image digests the exact pin must never accept: ANE1 from the T8152
 * source record, H15 T8122 from the accepted H15 pins header. No extra
 * engine option exists to accept them. */
static const uint8_t ane1_payload_sha[32] = {
	0x5d,0x3e,0xd4,0xc7, 0x28,0x81,0xe1,0x6f, 0x89,0xe5,0x76,0x2b,
	0x19,0x4d,0xef,0x79, 0x88,0x2d,0xb5,0x4c, 0xfe,0x56,0xef,0xb7,
	0x32,0xff,0xab,0x20, 0x92,0x2d,0x14,0x77
};
static const uint8_t h15_t8122_payload_sha[32] = {
	0x19,0xb6,0xa4,0x99, 0x97,0xec,0xf3,0xeb, 0x4e,0xc1,0x59,0x60,
	0xf4,0x2a,0xb6,0xa7, 0x9f,0xec,0xd3,0x4c, 0x8e,0x14,0x48,0xd2,
	0xe5,0xc1,0x03,0x4e, 0x2e,0xc4,0x78,0x08
};

static unsigned char *read_whole(const char *path)
{
	FILE *f = fopen(path, "rb");
	unsigned char *buf;
	size_t got;

	if (!f)
		err(1, "open %s", path);
	buf = malloc(PAYLOAD_SIZE);
	if (!buf)
		err(1, "malloc");
	got = fread(buf, 1, PAYLOAD_SIZE, f);
	if (got != PAYLOAD_SIZE || fgetc(f) != EOF)
		errx(1, "%s: expected exactly %llu payload bytes", path,
		     (unsigned long long)PAYLOAD_SIZE);
	fclose(f);
	return buf;
}

static void seal(const struct ane_h18_firmware *img, uint8_t text[32],
		 uint8_t data[32])
{
	ane_sha256(img->storage, ANE_H18_FW_TEXT_SIZE, text);
	ane_sha256((unsigned char *)img->storage + ANE_H18_FW_TEXT_SIZE,
		   ANE_H18_FW_DATA_FILE_SIZE, data);
}

int main(int argc, char **argv)
{
	struct ane_h18_firmware img = { 0 };
	const struct ane_h18_fw *pin = &ane_fw_t8152_ane0;
	unsigned char *payload, *synthetic;
	uint8_t fixture_digest[32], text0[32], data0[32], text1[32], data1[32];
	struct ane_h18_firmware fresh = { 0 };
	int ret;

	if (argc != 2)
		errx(1, "usage: %s <payload>", argv[0]);
	payload = read_whole(argv[1]);

	ane_sha256(payload, PAYLOAD_SIZE, fixture_digest);
	if (memcmp(fixture_digest, pin->sha256, 32))
		errx(1, "delivered payload does not match the compiled pin");
	if (strcmp(pin->name, "h18_ane0_fw_kirkland_j8xx") ||
	    pin->size != 1572864u || pin->manifest_size != 1572889u)
		errx(1, "compiled pin identity drifted");

	if (getenv("ANE_H18_TEST_EXPECT_ENOMEM")) {
		ret = ane_h18_firmware_stage(0x8152, 0, payload, PAYLOAD_SIZE,
					     &img);
		if (ret != -ENOMEM)
			errx(1, "seam: expected -ENOMEM, got %d", ret);
		if (img.storage || img.size || img.entry)
			errx(1, "seam: owner created on ENOMEM");
		free(payload);
		puts("ok: allocation-failure seam refuses (-ENOMEM, no owner)");
		return 0;
	}

	/* Production entry, real pinned payload. */
	ret = ane_h18_firmware_stage(0x8152, 0, payload, PAYLOAD_SIZE, &img);
	if (ret)
		errx(1, "stage: %d", ret);
	if (!img.storage || img.size != ANE_H18_FW_IMAGE_SIZE || img.entry != 0)
		errx(1, "owner fields");
	if ((uintptr_t)img.storage == (uintptr_t)payload)
		errx(1, "storage aliases input");
	if (memcmp(img.storage, payload + ANE_H18_FW_TEXT_FILE,
		   ANE_H18_FW_TEXT_SIZE))
		errx(1, "TEXT bytes");
	if (memcmp((unsigned char *)img.storage + ANE_H18_FW_TEXT_SIZE,
		   payload + ANE_H18_FW_DATA_FILE,
		   ANE_H18_FW_DATA_FILE_SIZE))
		errx(1, "DATA bytes");
	{
		size_t i;

		for (i = ANE_H18_FW_TEXT_SIZE + ANE_H18_FW_DATA_FILE_SIZE;
		     i < ANE_H18_FW_IMAGE_SIZE; i++)
			if (((unsigned char *)img.storage)[i])
				errx(1, "zero tail dirty at %#zx", i);
	}
	puts("ok: staged TEXT/DATA byte-exact, zero tail, entry 0, size 0x370000");

	/* Wrong digest refuses before any allocation; the fresh owner stays
	 * zeroed. Flip a real byte so the compare sees real firmware bytes. */
	payload[0] ^= 0xff;
	ret = ane_h18_firmware_stage(0x8152, 0, payload, PAYLOAD_SIZE, &fresh);
	if (ret != -EBADMSG)
		errx(1, "tampered payload: %d", ret);
	if (fresh.storage || fresh.size || fresh.entry)
		errx(1, "owner created on EBADMSG");
	payload[0] ^= 0xff;
	puts("ok: one flipped firmware byte refused (-EBADMSG, no owner)");

	seal(&img, text0, data0);

	/* Exact size gate fires before the busy check, and nulls refuse. */
	synthetic = malloc(PAYLOAD_SIZE);
	if (!synthetic)
		err(1, "malloc");
	memset(synthetic, 0xa5, PAYLOAD_SIZE);
	if (ane_h18_firmware_stage(0x8152, 0, synthetic, PAYLOAD_SIZE - 1,
				   &img) != -EINVAL)
		errx(1, "short size accepted");
	if (ane_h18_firmware_stage(0x8152, 0, synthetic, PAYLOAD_SIZE + 1,
				   &img) != -EINVAL)
		errx(1, "long size accepted");
	if (ane_h18_firmware_stage(0x8152, 0, NULL, PAYLOAD_SIZE, &img)
	    != -EINVAL)
		errx(1, "null payload accepted");
	if (ane_h18_firmware_stage(0x8152, 0, synthetic, PAYLOAD_SIZE, NULL)
	    != -EINVAL)
		errx(1, "null out accepted");
	/* Busy owner refuses before digest or allocation work. */
	if (ane_h18_firmware_stage(0x8152, 0, synthetic, PAYLOAD_SIZE, &img)
	    != -EBUSY)
		errx(1, "busy owner not refused");
	/* Only ownchip ANE0: wrong chip, ANE1, and an H15 soc all refuse. */
	if (ane_h18_firmware_stage(0x8153, 0, synthetic, PAYLOAD_SIZE, &img)
	    != -ENODEV)
		errx(1, "wrong soc accepted");
	if (ane_h18_firmware_stage(0x8152, 1, synthetic, PAYLOAD_SIZE, &img)
	    != -ENODEV)
		errx(1, "ANE1 accepted");
	if (ane_h18_firmware_stage(0x6031, 0, synthetic, PAYLOAD_SIZE, &img)
	    != -ENODEV)
		errx(1, "H15 soc accepted");
	/* The exact pin rejects any other digest; it is neither the ANE1 nor
	 * the H15 T8122 digest. */
	ret = ane_h18_firmware_stage(0x8152, 0, synthetic, PAYLOAD_SIZE,
				     &fresh);
	if (ret != -EBADMSG)
		errx(1, "synthetic payload: %d", ret);
	if (fresh.storage || fresh.size || fresh.entry)
		errx(1, "owner created on synthetic EBADMSG");
	ane_sha256(synthetic, PAYLOAD_SIZE, fixture_digest);
	if (!memcmp(fixture_digest, pin->sha256, 32))
		errx(1, "synthetic matched pin");
	if (!memcmp(pin->sha256, ane1_payload_sha, 32))
		errx(1, "pin equals the ANE1 digest");
	if (!memcmp(pin->sha256, h15_t8122_payload_sha, 32))
		errx(1, "pin equals the H15 digest");
	free(synthetic);
	puts("ok: exact-pin refusals (size, nulls, busy, soc/engine, ANE1/H15 digests)");

	/* The existing owner survived every refusal unchanged. */
	seal(&img, text1, data1);
	if (memcmp(text0, text1, 32) || memcmp(data0, data1, 32))
		errx(1, "owner changed by refusals");

	/* Independence: live mutations in both copied source spans, asserted
	 * before the free, then after the free. The owner must not move. */
	memset(payload + ANE_H18_FW_TEXT_FILE, 0x5a, 4096);
	seal(&img, text1, data1);
	if (memcmp(text0, text1, 32) || memcmp(data0, data1, 32))
		errx(1, "owner followed TEXT-span input mutation");
	memset(payload + ANE_H18_FW_DATA_FILE, 0xa7, 4096);
	seal(&img, text1, data1);
	if (memcmp(text0, text1, 32) || memcmp(data0, data1, 32))
		errx(1, "owner followed DATA-span input mutation");
	free(payload);
	seal(&img, text1, data1);
	if (memcmp(text0, text1, 32) || memcmp(data0, data1, 32))
		errx(1, "owner followed freed input");
	puts("ok: owner independent of TEXT/DATA-span mutations and freed input");

	ane_h18_firmware_free(&img);
	if (img.storage || img.size || img.entry)
		errx(1, "free left owner");
	ane_h18_firmware_free(&img);
	ane_h18_firmware_free(NULL);
	puts("ok: release clears and is idempotent");
	puts("PASS h18 ane0 owned-image host staging");
	return 0;
}
