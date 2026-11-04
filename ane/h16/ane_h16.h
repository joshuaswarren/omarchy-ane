/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * ane_h16.h: per-SoC table for the H16/H17 (27.0 macOS) ANE family.
 *
 * Every value carries its source. "ADT j604ap" means the macOS 27.0
 * (26A428) device tree ANE node for the j604ap board (logs in
 * receipts/2026-10-03-ane-h16/); "macOS" means
 * the macOS 27.0 behaviour of the H16/H17 ANE driver. Its
 * ane-type 0xa0
 * (T6021) row reproduces the known T6021 map (SCRATCH0 0x1840048,
 * CPU_CONTROL 0x1400044), which calibrates the rest of the table.
 * "iBoot" means the iBoot coprocessor table entry id 4.
 *
 * AneH16Driver owns this file. AneH17Driver adds rows in
 * ane/h17/ane_h17_soc.h and the of_match entries in ane_h16.c.
 */
#ifndef __ANE_H16_SOC_H__
#define __ANE_H16_SOC_H__

#include <linux/types.h>

/* One pinned firmware payload (the payload bytes, not the wrapper).
 * Sizes/hashes from receipts/2026-10-03-ane-h16; both payloads are
 * unencrypted raw arm64e images, RTKit 3514.0.15.
 * text_vmsize: text segment vm size; data_vm/vmsize/fileoff/filesize:
 * data segment placement (the zerofill tail beyond data_filesize stays
 * zero in the staged copy); patchbay_vm/tunables_vm: data segment
 * sections whose preload bytes are taken from the iBoot copy. */
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
	u32 ce_env_vm;
	u32 ce_env_size;
};

/* Offsets are engine-relative unless a field name ends in _pa. */
struct ane_h16_soc {
	const char *name;	/* opt-in key and log name, e.g. "t8132" */
	u32 ane_type;		/* ADT ane-type, cross-checked at probe */
	u32 cpu_control;	/* write 0 then 0x10 */
	u32 cpu_status;		/* low word of the status register */
	u32 rvbar;		/* read as one 64-bit value */
	u32 scratch0;		/* SCRATCHn = scratch0 + 4n */
	u32 irq_status;		/* IRQ pending register */
	u32 irq_ack;		/* IRQ acknowledge register */
	u32 mbox;		/* iBoot coproc table reg table: +0x8000
				 * (a2i/i2a ctrl 0x110/0x114, a2i send
				 * 0x800/0x808, i2a recv 0x830/0x838) */
	u64 wrapper_pa;		/* iBoot coproc entry +8 (engine+0x1600000) */
	u64 cpu_pa;		/* iBoot coproc entry +0x20 (engine+0x1000000) */
	u32 ps_off[5];		/* pmgr ps words ANE_SYS, ANE_MPM, ANE_CPU,
				 * ANE_TD, ANE_BASE, offsets into the pmgr
				 * reg window the DT node names "pmgr" */
	u8 ps_count;		/* 0: all five legacy slots; otherwise valid prefix */
	const struct ane_h16_fw *fw;
};

extern const struct ane_h16_soc ane_t8132_soc;
extern const struct ane_h16_soc ane_t6040_soc;
extern const struct ane_h16_soc ane_t6041_soc;
extern const struct ane_h16_soc ane_t8140_soc;

#endif /* __ANE_H16_SOC_H__ */
