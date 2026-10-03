/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * ane_h15.h — per-SoC table for the H15 (M3) family ANE. Every value
 * carries its source. "ADT 27.0" = the macOS 27.0 IPSW ADT summary
 * (receipts/2026-10-03-ane-h15/README.md; source artifact
 * artifacts/AneAllSoc/every-soc/receipt/adt-27.0.txt, sha256
 * 04fe38b8…); "ADT 13.5" = the macOS 13.5 page-size note (the T8110
 * family is the only T8110 fact extracted). "analyzer" =
 * receipts/2026-10-03-ane-h15 data/ane-soc/SOC.json (post-b0 corrected
 * records); "aurora" = aurora-silicon/linux asahi-7.1.13-3 tag
 * 94fb2334 dtsi files. Word tiers and the standing rule for what
 * this module may touch live in the receipt; the H16 module
 * (ane/h16/ane_h16.h) is the structural template.
 */
#ifndef __ANE_H15_SOC_H__
#define __ANE_H15_SOC_H__

#include <linux/types.h>
#include <crypto/sha2.h>
#include "ane_h15_adt.h"

/* One pinned firmware payload. The Mach-O vm layout (text/data
 * fileoff, patchbay/tunables) is NOT measured for any H15 image
 * (sources S2/S3: BuildManifest and IPSW payload, not the disassembled
 * image). The H16 module reuses the iBoot preload instead of staging
 * a file; that path is the H15 default too. fw_path= is the explicit
 * override the volunteer signs. */
struct ane_h15_fw {
	const char *name;
	u8 sha256[32];
	u32 size;		/* the size the SHA-256 was taken over (S3) */
	u32 manifest_size;	/* the BuildManifest (S2) size; differs by 25 */
};

/* word group: name, where, offset, size (4 or 8 bytes), tier, source.
 * The module's stages check tier, not the offsets; offset is for the
 * log. */
enum ane_h15_where {
	ANE15_WHERE_PMGR = 0,	/* the kernel-mapped pmgr window */
	ANE15_WHERE_ENGINE = 1,	/* inside the engine aperture */
};

enum ane_h15_tier {
	/* MEASURED address, MEASURED semantics, kernel-mapped window: read
	 * at stage=status behind genpd and the ACTUAL=0xf guard. */
	ANE15_TIER_MEASURED_GUARD,
	/* MEASURED address (ADT reg window), INFERENCE role: the H16 b0
	 * table lists these with "address MEASURED; M3 role INFERENCE".
	 * Stage=wrapper reads them only when the soc row's cleared flag is
	 * true; H15 soc rows all leave it false (REFUSED, see below). */
	ANE15_TIER_ADDR_MEASURED,
	/* Address INFERRED, role INFERRED: refused at every stage. */
	ANE15_TIER_INFERRED,
	/* T6021 fatal range and CoreSight-style offsets: never. */
	ANE15_TIER_FORBIDDEN,
};

struct ane_h15_word {
	const char *name;
	u8 where;
	u32 off;	/* engine-relative for ENGINE, pmgr-relative for PMGR */
	u8 size;	/* 4 or 8 */
	u8 tier;
	bool cleared;	/* a macOS capture has measured the read on H15 */
	const char *src;	/* short evidence label for the log */
};

/* One SoC row. */
struct ane_h15_soc {
	const char *name;	/* opt-in key and log name */
	u32 ane_type;		/* ADT ane-type, cross-checked at probe */
	u32 cpu_control;	/* wrapper+0x44 (engine-relative) */
	u32 cpu_status;		/* wrapper+0x48 */
	u32 rvbar;		/* engine+0x1050000 */
	u32 mbox;		/* wrapper+0x8000 (INFERENCE on H15) */
	u64 engine_pa;		/* engine window base address */
	u32 engine_size;	/* engine window size */
	u64 pmgr_pa;		/* pmgr window base address */
	u32 pmgr_size;		/* pmgr window size */
	const u32 *ps_off;	/* 5 pmgr-relative offsets: SYS MPM CPU TD BASE */
	const struct ane_h15_word *words;
	const struct ane_h15_fw *fw;
};

extern const struct ane_h15_soc ane_t8122_soc;
extern const struct ane_h15_soc ane_t6030_soc;
extern const struct ane_h15_soc ane_t6031_soc;
extern const struct ane_h15_soc ane_t6034_soc;

#endif /* __ANE_H15_SOC_H__ */
