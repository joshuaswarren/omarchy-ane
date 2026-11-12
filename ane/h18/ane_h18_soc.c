// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane_h18_soc.c: the T8152 ANE0 row. Sources in ane_h18.h. ANE1 (ADT /arm-io/ane1: asc 0x481600000,
 * rvbar 0x481050000, engine 0x480000000, mailbox number 13, PSD slots 32 and 33) is not a row yet: the
 * overlay does not describe it and no stage has run on ANE0.
 */
#include "ane_h18.h"

const struct ane_h18_soc ane_t8152_soc = {
	.name = "t8152",
	.ane_type = 784,
	.row_complete = true,
	.asc_pa = 0x419600000ULL,
	.asc_size = 0x74000,
	.rvbar_pa = 0x419050000ULL,
	.rvbar_size = 0x4000,
	.mbox_pa = 0x316000000ULL,
	.mbox_size = 0x1c000,
	.engine_pa = 0x418000000ULL,
	.engine_size = 0x2000000,
	.scratch_pa = 0x4198a0000ULL,
	.scratch_size = 0x1000,
	.dart_pa = 0x419800000ULL,
	.dart_size = 0xc000,
	.mbox_num = 12,
	.rvbar_expect = 0x10000000001ULL,
	.psd_slot = { 30, 31 },
	/* /arm-io/ane segment-ranges of the saved M6 ADT: TEXT 0x100014e8000 (0xc4000) at IOVA 0x10000000000, DATA 0x100030c4000 (0x2ac000) at
	 * IOVA 0x10000000000 + 0xc4000. IORVBAR 0x10000000001 is the TEXT IOVA with the valid bit. */
	.fw_seg = {
		{ .pa = 0x100014e8000ULL, .size = 0xc4000, .iova = 0x10000000000ULL, .write = false },
		{ .pa = 0x100030c4000ULL, .size = 0x2ac000, .iova = 0x100000c4000ULL, .write = true },
	},
};

/*
 * T8132 (K114c, M5 Max) ANE0 row. ADT ane0@8000000: ane-type 0x222, seven MMIO windows
 * ([0] 0x5080000000/32M engine, [1] 0x280020000/96K, [2] 0x280060000/16K, [3] 0x284000000/256K,
 * [4] 0x212600000/~16M, [5] 0x5880000000/32M, [6] 0x288030000/16K), interrupts 0x3ba/0x3c7,
 * dart-ane0@9800000 compatible "dart,t8110" (mainlined as apple,t8110-dart) with mappers
 * iso1/iso2. Windows [1]/[3] as mbox/asc are inferred from the T8152 shape (its mbox and asc
 * are separate regions of similar size), not hardware-verified.
 *
 * The register offsets the shipped kernel extension of this SoC uses match the T8152 row
 * where the two can be compared: IORVBAR sits at engine+0x1050000 exactly like the T8152
 * row's rvbar_pa, while the SCRATCH7 candidates differ by which branch serves which
 * codename, so both stay in the stage=reads list. The engine is exclave/PMP-managed on
 * this SoC: the ADT has no ps-ane node and no PSD slot number is derivable, and the
 * RTBuddy endpoint table is keyed on ane-type, not a Linux mbox number, so mbox_num
 * stays 0 until a hardware check names it. row_complete stays false until that check
 * validates the inferred windows.
 */
const struct ane_h18_soc ane_t8132_soc = {
	.name = "t8132",
	.ane_type = 546,
	.row_complete = false,
	.asc_pa = 0x284000000ULL,	/* ADT window [3]; inferred role */
	.asc_size = 0x40000,
	.rvbar_pa = 0x5081050000ULL,	/* engine + 0x1050000, the T8152-consistent offset */
	.rvbar_size = 0x4000,
	.mbox_pa = 0x280020000ULL,	/* ADT window [1]; inferred role */
	.mbox_size = 0x18000,
	.engine_pa = 0x5080000000ULL,	/* ADT window [0], the ane node's own base */
	.engine_size = 0x2000000,
	.scratch_pa = 0x5080000000ULL,	/* SCRATCH7 candidates sit inside the engine window */
	.scratch_size = 0x2000000,
	.dart_pa = 0x509000000ULL,	/* ADT dart-ane0 reg[0], the default mapper */
	.dart_size = 0xc000,
	.mbox_num = 0,			/* unknown: no ADT property names it */
	.rvbar_expect = 0x10000000001ULL, /* TEXT IOVA | valid; the plain convention that boots on the T8152 */
	.psd_slot = { 0, 0 },		/* exclave/PMP-managed on this SoC */
	.fw_seg = {
		{ .pa = 0x10000de8000ULL, .size = 0xec000, .iova = 0x10000000000ULL, .write = false },
		{ .pa = 0x100022c4000ULL, .size = 0x280000, .iova = 0x100000ec000ULL, .write = true },
	},
};
