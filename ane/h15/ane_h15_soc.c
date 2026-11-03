// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane/h15/ane_h15_soc.c — per-SoC rows for the H15 (M3) ANE family.
 * Sources in ane_h15.h and data/ane-soc/{t8122,t6030,t6031,t6034}.json
 * (post-b0 corrected records, receipts/2026-10-03-ane-h15).
 */
#include <linux/array_size.h>
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


const struct ane_h15_soc ane_t8122_soc = {
	.name = "t8122",
	.ane_type = 176,
	.cpu_control = 0x1400044,
	.cpu_status  = 0x1400048,
	.rvbar       = 0x1050000,
	/* Measured by stage 2 on a 13-inch M3 MacBook Air (J613), 2026-11-02: RVBAR_LO 0x1, RVBAR_HI 0x100,
	 * CPU_STATUS 0x30002a, both mailbox CTRL words 0x20001. The same word the T6030 and T6031 rows carry. */
	.rvbar_expect = 0x0000010000000001ull,
	.dart_off    = 0x1800000,	/* ADT S1: 0x311800000 - engine, data/ane-soc/t8122.json */
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
	/* Measured by stage 2 on a 16-inch M3 Pro (J516S), PR 175 comment of 2026-10-23: RVBAR_LO 0x1,
	 * RVBAR_HI 0x100. The same value the M2 Max run, the M6 (T8152) and the T6031 row carry: the
	 * firmware entry IOVA 0x10000000000 with bit 0 set, i.e. the boot loader already wrote it. T6034
	 * has no stage 2 result yet, so its rvbar_expect stays 0 and stage=run refuses on it. */
	.rvbar_expect = 0x0000010000000001ull,
	.dart_off    = 0x1800000,	/* ADT S1: 0x309800000 - engine, data/ane-soc/t6030.json */
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
	/* Measured by stage 2 on a 16-inch M3 Max (J516C), issue 176 comments of 2026-10-28: RVBAR_LO 0x1,
	 * RVBAR_HI 0x100, the same value as the T6030 row: the firmware entry IOVA 0x10000000000 with bit 0
	 * set, i.e. iBoot already wrote it. Two boots read the same word, and the ADT firmware ranges did
	 * not move between them. Stage 5 with map_fw=1 uses the T6031 DART overlay, never booted. */
	.rvbar_expect = 0x0000010000000001ull,
	.dart_off    = 0x1800000,	/* ADT S1: 0x3c9800000 - engine, data/ane-soc/t6031.json */
	.mbox        = 0x1408000,	/* INFERENCE */
	.engine_pa   = 0x3c8000000ull,
	.engine_size = 0x2000000,
	.pmgr_pa     = 0x292280000ull,
	.pmgr_size   = 0x10000,
	/* PWGATE: ADT ane0 reg entry 5 of the macOS 27.0 device tree for
	 * the J516C (0x292290000, 0x4000, next to the pmgr window).
	 * Shipped-system behaviour: the 0x199c word is cleared and
	 * polled before the core is released. Never run on an M3; the
	 * step is opt-in per run (pwgate_t6031). */
	.pwgate_pa   = 0x292290000ull,
	.pwgate_size = 0x4000,
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

/* Family-wide unmeasured facts for stage=boot (holes H1-H5 of the
 * M3/M4 plan section 1.5). `filled=false` means no MEASURED artifact
 * backs the row yet; stage=boot refuses while any row is unfilled and
 * prints one line per unfilled row. A later PR flips a row only with
 * the receipt named in `fill`. NO row may be marked filled without
 * that receipt: this table is the machine-checkable form of
 * "stage=boot stays REFUSED on H15".
 */
const struct ane_h15_fact ane_h15_facts[] = {
	{ "H1", "H15 stub-OS firmware identity (which firmware image iBoot preloads)",
	  "omarchy-ane-probe os-fw-version + omarchy-ane-firmware-fetch pin row",
	  false },
	{ "H2", "mailbox base + IRQ roles",
	  "S2 probe /proc/interrupts + S3 raw mbox CTRL reads", false },
	{ "H3", "firmware segment-ranges + DART sid/vm-base/vm-size",
	  "owner IODeviceTree capture (ane + dart-ane) beside the b0 receipts",
	  false },
	{ "H4", "RVBAR entry compose value",
	  "macOS behaviour on the H15, then S3 live observation",
	  false },
	{ "H5", "SCRATCH wake word + protocol role (never reuse T6021 0xf7fbdff9)",
	  "same macOS behaviour as H4, then live observation", false },
};

const unsigned int ane_h15_n_facts = ARRAY_SIZE(ane_h15_facts);
