/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef ANE_H18_FW_PINS_H
#define ANE_H18_FW_PINS_H

/* Exact 26A428 T8152 ANE0 identity shared by the data record and the host
 * stager. The IM4P wrapper is 1572889 bytes; the payload occupies
 * [25, 1572889). The whole-payload SHA-256 pins the segment layout, so no
 * parser or option flags are checked. No field of this struct selects
 * resources or qualifies an engine. ANE1 (h18_ane1_fw_kirkland_j8xx) is a
 * separate image and has no pin here. */
struct ane_h18_fw {
	const char *name;
	unsigned char sha256[32];	/* payload */
	unsigned int size;		/* payload bytes */
	unsigned int manifest_size;	/* IM4P wrapper bytes */
	unsigned char image_sha256[32];	/* IM4P wrapper */
};

static const struct ane_h18_fw ane_fw_t8152_ane0 = {
	.name = "h18_ane0_fw_kirkland_j8xx",
	.sha256 = { 0x83,0x05,0x05,0xbc, 0x95,0x78,0x6d,0xa1, 0xbf,0x49,0x9b,0xa9,
		    0x5b,0x6d,0x3c,0xf1, 0xd0,0x0a,0x5f,0x4e, 0x8e,0xfe,0x8f,0xe7,
		    0xda,0xb4,0x82,0x10, 0x14,0xb2,0x3d,0x72 },
	.size = 1572864,
	.manifest_size = 1572889,
	.image_sha256 = { 0x3a,0x1b,0xb2,0x64, 0x39,0xc2,0x8a,0x14, 0x65,0x46,0x81,0x0d,
			  0xe8,0x09,0x67,0x17, 0xad,0xa8,0xb8,0xeb, 0x80,0x52,0x15,0x63,
			  0x30,0xc5,0xb1,0x6d, 0x02,0x12,0x93,0xb8 }
};

#endif
