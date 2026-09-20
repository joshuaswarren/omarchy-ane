/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * ane_t6021_boot.h — T6021 legacy-boot contract units (pure, shared by
 * the kernel sequencer and the userspace regression).
 *
 * Byte anchors (KC kernelcache.release.mac14j sha256 8304156f…, selene
 * sha256 9f7915c4…; receipts under ane-linux-experiments/receipts/,
 * commits 3762aee + 12be074):
 *  - RVBAR fold: ANEHWDevice::ANE_Init 0x95e9850–0x95e9988 (K13
 *    0x93203b0–0x93204e0 identical). Read64 of eng+0x01050000; bit0
 *    set skips the write (live jw14m2 read 0x1). Otherwise write64
 *    ENTRY_BASE | (fw DVA & ADDR_MASK) — single u64 store
 *    (2026-09-20-h14-rvbar-width.json width_proof).
 *  - fw DVA = ANESharedMemorySurfaceParams("FWIM")+0x18, produced by
 *    dartMapMemoryDescriptor (vtable slot 0x8a8, genIOVMSegments) for
 *    the out-param installed at start+0x336c (mapper-callchain audit
 *    pass1–3). Linux correspondence: dma_alloc_coherent on the ane
 *    platform device is dart-ane0-translated (live iommu group 6,
 *    three apple,t6020-dart streams, DMA domain).
 *  - SCRATCH0/1 u64 split: low32 → 0x01840048 first, high32 →
 *    0x0184004c, dsb st before (legacy-init-publication receipt,
 *    accessor write32 width-proven at 0x9622d4c).
 *  - 0x174 init suballocation: header[0x00] u64 = fw DVA (legacy
 *    branch, dev+0x780 bit0 clear = observed AppleARMIODevice
 *    provider), [0x68] u32 = 64 count, template 256 B at
 *    [0x6C,0x16C) with template+0xC0 = 4 RESOLVED (Main raw anchors
 *    0x9612b78/7c/80 on the dev+0x998 template; conditional |=0x10
 *    separate, unqualified, not set).
 *
 * This header intentionally includes nothing. Kernel consumers have
 * <linux/types.h> already; userspace consumers typedef u8/u32/u64 and
 * include <stdbool.h> first (tools/h14_boot_regression.c in this
 * repository is the shipped runnable check).
 */
#ifndef __ANE_T6021_BOOT_H__
#define __ANE_T6021_BOOT_H__

/* RVBAR entry fold (ANE_Init 0x95e9868–0x95e988c). The mask clears
 * bits 0–10, 48 and 55 — bit 11 IS retained (0xf800 keeps 0x800); the
 * base supplies bit0 and the fixed upper pattern. */
#define ANE_T6021_RVBAR_ENTRY_BASE	0x0081000000000001ULL
#define ANE_T6021_RVBAR_ADDR_MASK	0xff7efffffffff800ULL

/* CPU_CONTROL release (rvbar-width local order: write32 0 then 0x10;
 * h14g config selects eng+0x01400044). RUN = BIT(4). */
#define ANE_T6021_CPU_RUN_RELEASE	0x10

/* SCRATCH7 handshake words (InitializeRTBuddy / legacy ANE_Init
 * publication; selene ack writer 0x77c8). */
#define ANE_T6021_BOOT_WAKE_REQ		0xf7fbdff9U
#define ANE_T6021_BOOT_ACK		0x08042006U

/* Init suballocation geometry (ANE_Init 0x95ea710–0x95ea97c;
 * legacy-init-publication receipt). */
#define ANE_T6021_INIT_STRUCT_SIZE	0x174
#define ANE_T6021_INIT_FW_DVA_OFF	0x00	/* u64, legacy branch */
#define ANE_T6021_INIT_COUNT_OFF	0x68	/* u32 = 64 */
#define ANE_T6021_INIT_COUNT		0x40
#define ANE_T6021_INIT_TEMPLATE_OFF	0x6c	/* 256 B at [0x6C,0x16C) */
#define ANE_T6021_INIT_TEMPLATE_SIZE	0x100
#define ANE_T6021_INIT_TBIT_OFF		0xc0	/* template-relative */
#define ANE_T6021_INIT_TBIT_VAL		0x4	/* RESOLVED (Main raw
					 * anchors 0x9612b78/7c/80:
					 * ldr/orr#4/str on the dev+0x998
					 * template); the conditional
					 * |=0x10 is a separate, still
					 * unqualified mutation and is
					 * NOT set */

/* Main-corrected (2026-09-20) open fields — the fill() leaves them
 * zero and publication cannot fire over them: [0x08] (=
 * *(dev+0x988+0x18), Params-pattern DVA of a second surface, identity
 * undecoded), [0x10] (= config size), [0x18] (= 0x10000000-config.size),
 * [0x20], [0x28], [0x30] (load-progress word). */
#define ANE_T6021_INIT_STRUCT_SIZE	0x174

static inline u64 ane_t6021_rvbar_compose(u64 iova)
{
	return ANE_T6021_RVBAR_ENTRY_BASE | (iova & ANE_T6021_RVBAR_ADDR_MASK);
}

/* The kext skip condition: bit0 of the RVBAR read (tbnz w0,#0 at
 * ANE_Init 0x95e9878; live jw14m2 reads have been 0x1). What a set
 * bit names in boot-mode terms is the open lifecycle fork — see
 * ane_t6021_boot.c; this predicate only decodes the bit. */
static inline bool ane_t6021_rvbar_latched(u64 rd)
{
	return rd & 1;
}

/* Entry bits of an RVBAR read (what the fold would have named). */
static inline u64 ane_t6021_rvbar_entry_bits(u64 rd)
{
	return rd & ANE_T6021_RVBAR_ADDR_MASK;
}

/* Boot acceptance predicate — the EXACT check the boot path applies
 * to a staged fw DVA: the fold must lose nothing. Bit 11 is retained;
 * bits 0-10, 48 and 55 are not, so an iova with any of them set is
 * rejected rather than silently truncated. */
static inline bool ane_t6021_rvbar_entry_ok(u64 iova)
{
	return (iova & (u64)~ANE_T6021_RVBAR_ADDR_MASK) == 0;
}

/* DISPUTED template region — do NOT write: the template-allocation/
 * def-use receipts decoded an initial dev+0x998 template with
 * word+0xC0 = 4, but pass5's legacy-path analysis reads the template
 * zeros; the contradiction is unresolved pending pass5b raw evidence
 * of the actual template producer. Until it lands, the fill writes
 * nothing in [0x6C,0x16C) and makes no template claim. */

/* Publication convention: low32 → SCRATCH0 (0x01840048) first, then
 * high32 → SCRATCH1, after dsb st. */
static inline void ane_t6021_scratch64_split(u64 v, u32 *lo, u32 *hi)
{
	*lo = (u32)(v & 0xffffffffU);
	*hi = (u32)(v >> 32);
}

static inline u64 ane_t6021_scratch64_join(u32 lo, u32 hi)
{
	return ((u64)hi << 32) | lo;
}

/* Fill the CLOSED fields of a ZEROED 0x174-byte init suballocation.
 * The caller owns zeroing (dma_alloc_coherent memory is zero).
 *
 * Field table (pass5 c364f24 + Main correction 2026-09-20: the audit's
 * "+0x24" was DECIMAL 24 = 0x18, and x9 is overwritten before the
 * stp): the fw CONSUMES [0x08]..[0x68]. Producers:
 *   [0x08] = *(dev+0x988+0x18) — a Params-pattern DVA of a SECOND
 *            surface (dev+0x988 identity still undecoded),
 *   [0x10] = config size (the fw payload length; equivalence to
 *            ANE_FW_BLOB_SIZE on Linux unconfirmed),
 *   [0x18] = 0x10000000-config.size (formula closed),
 *   [0x20]/[0x28] open, [0x30] = dev+0x990 load-progress word.
 * Template+0xC0 = 4 is RESOLVED (Main raw anchors 0x9612b78/7c/80).
 * The remaining open fields have no pinned Linux source, so this fill
 * writes [0x00], [0x68] and template+0xC0 only, leaves the rest
 * untouched, and the result is NOT a valid init structure: zeros are
 * UNSAFE in the fw-consumed zone and publication stays fenced. */
static inline void ane_t6021_init_struct_fill(u8 *buf, u64 fw_iova)
{
	buf[ANE_T6021_INIT_FW_DVA_OFF + 0] = (u8)fw_iova;
	buf[ANE_T6021_INIT_FW_DVA_OFF + 1] = (u8)(fw_iova >> 8);
	buf[ANE_T6021_INIT_FW_DVA_OFF + 2] = (u8)(fw_iova >> 16);
	buf[ANE_T6021_INIT_FW_DVA_OFF + 3] = (u8)(fw_iova >> 24);
	buf[ANE_T6021_INIT_FW_DVA_OFF + 4] = (u8)(fw_iova >> 32);
	buf[ANE_T6021_INIT_FW_DVA_OFF + 5] = (u8)(fw_iova >> 40);
	buf[ANE_T6021_INIT_FW_DVA_OFF + 6] = (u8)(fw_iova >> 48);
	buf[ANE_T6021_INIT_FW_DVA_OFF + 7] = (u8)(fw_iova >> 56);

	buf[ANE_T6021_INIT_COUNT_OFF + 0] = (u8)ANE_T6021_INIT_COUNT;
	buf[ANE_T6021_INIT_COUNT_OFF + 1] = 0;
	buf[ANE_T6021_INIT_COUNT_OFF + 2] = 0;
	buf[ANE_T6021_INIT_COUNT_OFF + 3] = 0;

	/* template+0xC0 = 4: RESOLVED initial state (Main raw anchors
	 * 0x9612b78/7c/80). The conditional |=0x10 remains unset. */
	buf[ANE_T6021_INIT_TEMPLATE_OFF + ANE_T6021_INIT_TBIT_OFF + 0] =
		ANE_T6021_INIT_TBIT_VAL;
}

#endif /* __ANE_T6021_BOOT_H__ */
