/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * ane_h16.h — per-SoC table for the H16/H17 (27.0 kext) ANE family.
 *
 * Every value carries its source. "ADT j604ap" means the macOS 27.0
 * (26A428) IPSW DeviceTree.j604ap.adt ANE node (payload sha256
 * fa30e1c7…, logs in receipts/2026-10-03-ane-h16/); "kext" means
 * AppleH16ANEInterface 10.19.2 in kernelcache.release.mac16g
 * (decompressed sha256 803a5408…), ANEHWDeviceConfig::
 * initializeANESoCConfig at 0xfffffe0009616180 — the ane-type 0xa0
 * (T6021) row reproduces the known T6021 map (SCRATCH0 0x1840048,
 * CPU_CONTROL 0x1400044), which calibrates the field decode.
 * "iBoot" means iBoot.j604.RELEASE.im4p (mBoot-20457.1.29, payload
 * sha256 a53b4706…) coprocessor table entry id 4.
 *
 * AneH16Driver owns this file. AneH17Driver adds rows in
 * ane/h17/ane_h17_soc.h and the of_match entries in ane_h16.c.
 */
#ifndef __ANE_H16_SOC_H__
#define __ANE_H16_SOC_H__

#include <linux/types.h>

/* One pinned firmware payload (the IM4P OCTET STRING, not the wrapper).
 * Sizes/hashes from receipts/2026-10-03-ane-h16; both payloads are
 * unencrypted raw Mach-O (MH_PRELOAD, arm64e), RTKit 3514.0.15.
 * text_vmsize: __TEXT vm size; data_vm/vmsize/fileoff/filesize:
 * __DATA placement (the zerofill tail beyond data_filesize stays zero
 * in the staged copy); patchbay_vm/tunables_vm: __DATA sections whose
 * preload bytes are taken from the iBoot copy. */
struct ane_h16_fw {
	const char *name;		/* request_firmware() path */
	u8 sha256[32];
	u32 size;
	u32 text_vmsize;
	u32 data_vm;
	u32 data_vmsize;
	u32 data_fileoff;
	u32 data_filesize;
	u32 patchbay_vm;
	u32 tunables_vm;
};

/* Offsets are engine-relative unless a field name ends in _pa. */
struct ane_h16_soc {
	const char *name;	/* opt-in key and log name, e.g. "t8132" */
	u32 ane_type;		/* ADT ane-type, cross-checked at probe */
	u32 cpu_control;	/* kext dev+0x4a0: write 0 then 0x10 */
	u32 cpu_status;		/* kext dev+0x498 lo */
	u32 rvbar;		/* kext ANE_Init read64OneShot w1 imm */
	u32 scratch0;		/* SCRATCHn = scratch0 + 4n (kext dev+0x438) */
	u32 irq_status;		/* kext dev+0x438+0x50[2] */
	u32 irq_ack;		/* kext dev+0x438+0x50[3] */
	u32 mbox;		/* iBoot coproc table reg table: +0x8000
				 * (a2i/i2a ctrl 0x110/0x114, a2i send
				 * 0x800/0x808, i2a recv 0x830/0x838) */
	u64 wrapper_pa;		/* iBoot coproc entry +8 (engine+0x1600000) */
	u64 cpu_pa;		/* iBoot coproc entry +0x20 (engine+0x1000000) */
	u32 ps_off[5];		/* pmgr ps words ANE_SYS, ANE_MPM, ANE_CPU,
				 * ANE_TD, ANE_BASE, offsets into the pmgr
				 * reg window the DT node names "pmgr" */
	const struct ane_h16_fw *fw;
};

extern const struct ane_h16_soc ane_t8132_soc;
extern const struct ane_h16_soc ane_t6040_soc;
extern const struct ane_h16_soc ane_t6041_soc;

#endif /* __ANE_H16_SOC_H__ */
