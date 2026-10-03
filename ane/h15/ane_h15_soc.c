// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane/h15/ane_h15_soc.c — per-SoC rows for the H15 (M3) ANE family.
 * Sources in ane_h15.h and data/ane-soc/{t8122,t6030,t6031,t6034}.json
 * (post-b0 corrected records, receipts/2026-10-03-ane-h15).
 */
#include "ane_h15.h"

/* ANE_SYS, ANE_MPM, ANE_CPU, ANE_TD, ANE_BASE offsets into the pmgr
 * window. The H15 b0 table measures every address; the T6021
 * semantics (TARGET/ACTUAL 0xf = on) carry over per receipts/2026-09-30
 * and the T6021 program rule. T6034 = T6031 (data record groups
 * j514m/j516m under arm-io,t6031). */
static const u32 ane_h15_ps_8122[] = { 0x438, 0xc000, 0xc008, 0xc010, 0xc018 };
static const u32 ane_h15_ps_6030[] = { 0x498, 0xc000, 0xc008, 0xc010, 0xc018 };
static const u32 ane_h15_ps_6031[] = { 0x520, 0x5a0, 0x5a8, 0x5b8, 0x5c0 };

/* T6021 fatal-read range and CoreSight-style offsets (ane/t6021/
 * ane_t6021.h: ANE_FATAL_READ_LO/HI = 0x1854000..0x1c04000). H15
 * inherits the rule even though no H15 evidence clears a specific
 * range; the engine window is read-only on the H15 paths and any
 * word outside the SOC word table is refused. */
static const struct ane_h15_word ane_h15_words_8122[] = {
	{ "ANE_SYS",  ANE15_WHERE_PMGR,   0x438, 4, ANE15_TIER_MEASURED_GUARD,
	  true,  "ADT 27.0 (A1); T6021 §4 guard" },
	{ "ANE_MPM",  ANE15_WHERE_PMGR,   0xc000, 4, ANE15_TIER_MEASURED_GUARD,
	  true,  "ADT 27.0" },
	{ "ANE_CPU",  ANE15_WHERE_PMGR,   0xc008, 4, ANE15_TIER_MEASURED_GUARD,
	  true,  "ADT 27.0" },
	{ "ANE_TD",   ANE15_WHERE_PMGR,   0xc010, 4, ANE15_TIER_MEASURED_GUARD,
	  true,  "ADT 27.0" },
	{ "ANE_BASE", ANE15_WHERE_PMGR,   0xc018, 4, ANE15_TIER_MEASURED_GUARD,
	  true,  "ADT 27.0" },
	{ "CPU_STATUS", ANE15_WHERE_ENGINE, 0x1400048, 4,
	  ANE15_TIER_ADDR_MEASURED, false, "ADT 27.0 reg[0]; role INFERENCE" },
	{ "RVBAR",      ANE15_WHERE_ENGINE, 0x1050000, 8,
	  ANE15_TIER_ADDR_MEASURED, false, "ADT 27.0 reg[1]; role INFERENCE" },
	{ "mbox a2i_ctrl", ANE15_WHERE_ENGINE, 0x1408110, 4,
	  ANE15_TIER_INFERRED, false, "wrapper+0x8000 INFERENCE" },
	{ "mbox i2a_ctrl", ANE15_WHERE_ENGINE, 0x1408114, 4,
	  ANE15_TIER_INFERRED, false, "wrapper+0x8000 INFERENCE" },
	{ "MBI doorbell",  ANE15_WHERE_ENGINE, 0x1844000, 4,
	  ANE15_TIER_INFERRED, false, "T6021-only constant" },
	{ "CoreSight",     ANE15_WHERE_ENGINE, 0x1010000, 4,
	  ANE15_TIER_FORBIDDEN, false, "T6021 forbidden range" },
};

/* T6030 shares the engine+0x1400000 wrapper and engine+0x1050000
 * RVBAR with T8122 (same ADT block layout). pmgr is at 0x350700000. */
static const struct ane_h15_word ane_h15_words_6030[] = {
	{ "ANE_SYS",  ANE15_WHERE_PMGR,   0x498, 4, ANE15_TIER_MEASURED_GUARD,
	  true,  "ADT 27.0" },
	{ "ANE_MPM",  ANE15_WHERE_PMGR,   0xc000, 4, ANE15_TIER_MEASURED_GUARD,
	  true,  "ADT 27.0" },
	{ "ANE_CPU",  ANE15_WHERE_PMGR,   0xc008, 4, ANE15_TIER_MEASURED_GUARD,
	  true,  "ADT 27.0" },
	{ "ANE_TD",   ANE15_WHERE_PMGR,   0xc010, 4, ANE15_TIER_MEASURED_GUARD,
	  true,  "ADT 27.0" },
	{ "ANE_BASE", ANE15_WHERE_PMGR,   0xc018, 4, ANE15_TIER_MEASURED_GUARD,
	  true,  "ADT 27.0" },
	{ "CPU_STATUS", ANE15_WHERE_ENGINE, 0x1400048, 4,
	  ANE15_TIER_ADDR_MEASURED, false, "ADT 27.0 reg[0]; role INFERENCE" },
	{ "RVBAR",      ANE15_WHERE_ENGINE, 0x1050000, 8,
	  ANE15_TIER_ADDR_MEASURED, false, "ADT 27.0 reg[1]; role INFERENCE" },
	{ "mbox a2i_ctrl", ANE15_WHERE_ENGINE, 0x1408110, 4,
	  ANE15_TIER_INFERRED, false, "wrapper+0x8000 INFERENCE" },
	{ "mbox i2a_ctrl", ANE15_WHERE_ENGINE, 0x1408114, 4,
	  ANE15_TIER_INFERRED, false, "wrapper+0x8000 INFERENCE" },
	{ "MBI doorbell",  ANE15_WHERE_ENGINE, 0x1844000, 4,
	  ANE15_TIER_INFERRED, false, "T6021-only constant" },
	{ "CoreSight",     ANE15_WHERE_ENGINE, 0x1010000, 4,
	  ANE15_TIER_FORBIDDEN, false, "T6021 forbidden range" },
};

/* T6031: same offsets as T6030 except pmgr base (0x292280000) and the
 * 0xc000/0x5a0 split. The aurora t6031-pmgr.dtsi has all five ANE ps
 * nodes already; this row's ps_off points to them. */
static const struct ane_h15_word ane_h15_words_6031[] = {
	{ "ANE_SYS",  ANE15_WHERE_PMGR,   0x520, 4, ANE15_TIER_MEASURED_GUARD,
	  true,  "ADT 27.0" },
	{ "ANE_MPM",  ANE15_WHERE_PMGR,   0x5a0, 4, ANE15_TIER_MEASURED_GUARD,
	  true,  "ADT 27.0" },
	{ "ANE_CPU",  ANE15_WHERE_PMGR,   0x5a8, 4, ANE15_TIER_MEASURED_GUARD,
	  true,  "ADT 27.0" },
	{ "ANE_TD",   ANE15_WHERE_PMGR,   0x5b8, 4, ANE15_TIER_MEASURED_GUARD,
	  true,  "ADT 27.0" },
	{ "ANE_BASE", ANE15_WHERE_PMGR,   0x5c0, 4, ANE15_TIER_MEASURED_GUARD,
	  true,  "ADT 27.0" },
	{ "CPU_STATUS", ANE15_WHERE_ENGINE, 0x1400048, 4,
	  ANE15_TIER_ADDR_MEASURED, false, "ADT 27.0 reg[0]; role INFERENCE" },
	{ "RVBAR",      ANE15_WHERE_ENGINE, 0x1050000, 8,
	  ANE15_TIER_ADDR_MEASURED, false, "ADT 27.0 reg[1]; role INFERENCE" },
	{ "mbox a2i_ctrl", ANE15_WHERE_ENGINE, 0x1408110, 4,
	  ANE15_TIER_INFERRED, false, "wrapper+0x8000 INFERENCE" },
	{ "mbox i2a_ctrl", ANE15_WHERE_ENGINE, 0x1408114, 4,
	  ANE15_TIER_INFERRED, false, "wrapper+0x8000 INFERENCE" },
	{ "MBI doorbell",  ANE15_WHERE_ENGINE, 0x1844000, 4,
	  ANE15_TIER_INFERRED, false, "T6021-only constant" },
	{ "CoreSight",     ANE15_WHERE_ENGINE, 0x1010000, 4,
	  ANE15_TIER_FORBIDDEN, false, "T6021 forbidden range" },
};

/* Pinned 27.0 IPSW ANE payloads. S3 source: SHA-256 of the 1605632-
 * byte Mach-O payload after IM4P ASN.1 unwrap; payload bytes not
 * retained. BuildManifest (S2) reports 1605657; the 25-byte gap is
 * the same on every H15 row and is not explained locally. The module
 * checks against the S3 size+hash pair. */
static const struct ane_h15_fw ane_fw_t8122 = {
	.name = "h15_ane_fw_themis_j51y",
	.sha256 = { 0x19,0xb6,0xa4,0x99, 0x97,0xec,0xf3,0xeb, 0x4e,0xc1,0x59,0x60,
		    0xf4,0x2a,0xb6,0xa7, 0x9f,0xec,0xd3,0x4c, 0x8e,0x14,0x48,0xd2,
		    0xe5,0xc1,0x03,0x4e, 0x2e,0xc4,0x78,0x08 },
	.size = 1605632,
	.manifest_size = 1605657,
};

static const struct ane_h15_fw ane_fw_t6030 = {
	.name = "t603x_ane0_fw_erebus_ls5x",
	.sha256 = { 0x43,0xda,0x9d,0x56, 0x6a,0x88,0x0b,0xab, 0x1b,0xd8,0xef,0x9c,
		    0xd3,0xdd,0x3e,0xa4, 0xa5,0x5a,0xe6,0xc0, 0x16,0xd2,0xec,0x7f,
		    0x4f,0x97,0xf4,0xfd, 0xa9,0x98,0x67,0xa0 },
	.size = 1605632,
	.manifest_size = 1605657,
};

static const struct ane_h15_fw ane_fw_t6031 = {
	.name = "t603x_ane0_fw_erebus_pc5x",
	.sha256 = { 0xdd,0xac,0x37,0xc7, 0x0b,0xdc,0xae,0xdc, 0x62,0x39,0xd5,0xcc,
		    0x85,0xd8,0x48,0xe6, 0x06,0xcd,0x66,0xdf, 0x45,0x4e,0x8a,0x43,
		    0x2c,0x24,0xfd,0x77, 0x30,0xa1,0xf9,0x77 },
	.size = 1605632,
	.manifest_size = 1605657,
};

const struct ane_h15_soc ane_t8122_soc = {
	.name = "t8122",
	.ane_type = 176,
	.cpu_control = 0x1400044,
	.cpu_status  = 0x1400048,
	.rvbar       = 0x1050000,
	.mbox        = 0x1408000,	/* INFERENCE */
	.engine_pa   = 0x310000000ull,
	.engine_size = 0x2000000,
	.pmgr_pa     = 0x2d0700000ull,
	.pmgr_size   = 0x18000,
	.ps_off      = ane_h15_ps_8122,
	.words       = ane_h15_words_8122,
	.fw          = &ane_fw_t8122,
};

const struct ane_h15_soc ane_t6030_soc = {
	.name = "t6030",
	.ane_type = 192,
	.cpu_control = 0x1400044,
	.cpu_status  = 0x1400048,
	.rvbar       = 0x1050000,
	.mbox        = 0x1408000,	/* INFERENCE */
	.engine_pa   = 0x308000000ull,
	.engine_size = 0x2000000,
	.pmgr_pa     = 0x350700000ull,
	.pmgr_size   = 0x18000,
	.ps_off      = ane_h15_ps_6030,
	.words       = ane_h15_words_6030,
	.fw          = &ane_fw_t6030,
};

const struct ane_h15_soc ane_t6031_soc = {
	.name = "t6031",
	.ane_type = 224,
	.cpu_control = 0x1400044,
	.cpu_status  = 0x1400048,
	.rvbar       = 0x1050000,
	.mbox        = 0x1408000,	/* INFERENCE */
	.engine_pa   = 0x3c8000000ull,
	.engine_size = 0x2000000,
	.pmgr_pa     = 0x292280000ull,
	.pmgr_size   = 0x10000,
	.ps_off      = ane_h15_ps_6031,
	.words       = ane_h15_words_6031,
	.fw          = &ane_fw_t6031,
};

/* t6034 reuses t6031's ADT nodes and pmgr offsets (data record groups
 * j514m/j516m under arm-io,t6031); the compatible differs in DT. */
const struct ane_h15_soc ane_t6034_soc = {
	.name = "t6034",
	.ane_type = 224,
	.cpu_control = 0x1400044,
	.cpu_status  = 0x1400048,
	.rvbar       = 0x1050000,
	.mbox        = 0x1408000,	/* INFERENCE */
	.engine_pa   = 0x3c8000000ull,
	.engine_size = 0x2000000,
	.pmgr_pa     = 0x292280000ull,
	.pmgr_size   = 0x10000,
	.ps_off      = ane_h15_ps_6031,
	.words       = ane_h15_words_6031,
	.fw          = &ane_fw_t6031,
};
