/*
 * T8103 register map for the ane-add payload.
 *
 * Sources, all in this repo on the same branch:
 *   ane/src/ane_tm.c        TM/TQ window layout and the submit protocol
 *   ane/src/ane_drv.c       ane_submit(): BAR layout, qid=4, nid=0x40
 *   ane/src/uapi/drm/ane_accel.h   CMD_BUF_BDX=0, KRN_BUF_BDX=1,
 *                                ANE_CMD_GRAN=0x10, ANE_FIFO_NID=0x40,
 *                                ANE_TILE_COUNT=0x20
 *   packaging/dt/t8103-ane.dts     engine + three DARTs + PMGR chain
 *   pmgr word semantics: apple,pmgr-pwrstate (TARGET [3:0],
 *   ACTUAL [7:4], BUSY bit 11, DEV_DISABLE 10, PS_RESET 12,
 *   AUTO_ENABLE 28, WAS_CLKGATED 9, WAS_PWRGATED 8).
 *   DART (Gen1 / t8020 style): TTBR 0x200 (+idx*4 per sid, 4 per sid),
 *   TCR 0x100 (+sid*4), STREAMS_ENABLE 0xfc, STREAM_SELECT 0x34,
 *   STREAM_COMMAND 0x20. DART1 PTE: u64, pa[35:12] | SUBPAGE_END(0xfff
 *   at bits 51:40) | VALID bit0.
 *
 * HARD LIMIT (ane_tm.c bisect notes): the engine's own pmgr mirror
 * window (ane reg[1], PMGR + 0x8c000) is firmware-locked. A write
 * there external-aborts the SoC. This payload writes only the
 * power-controller words listed in PMGR_CHAIN below, never 0x8cxxx.
 */

#ifndef ANE_ADD_REGS_H
#define ANE_ADD_REGS_H

#define PMGR_BASE		0x23b700000UL
#define PMGR_PS_TARGET		0x0000000fU	/* bits [3:0]  */
#define PMGR_PS_ACTUAL		0x000000f0U	/* bits [7:4]  */
#define PMGR_PS_ACTIVE		0xfU
#define PMGR_BUSY		(1U << 11)
#define PMGR_DEV_DISABLE	(1U << 10)
#define PMGR_PS_RESET		(1U << 12)
#define PMGR_AUTO_ENABLE	(1U << 28)
#define PMGR_PS_FLAGS		((1U << 9) | (1U << 8))

/* Power chain, parents first (packaging/dt/t8103-ane.dts comment):
 * ane_sys -> ane_sys_cpu -> ane_base -> set1..5.  The DARTs hang off
 * ane_sys. */
static const unsigned int PMGR_CHAIN[8] = {
	0x470,				/* ane_sys      */
	0xc000,				/* ane_sys_cpu  */
	0xc008,				/* ane_base     */
	0xc010, 0xc018, 0xc020, 0xc028, 0xc030,	/* ane_set1..5  */
};

#define ANE_ENGINE		0x26bc04000UL	/* 0x24000 window  */
#define ANE_TM			(ANE_ENGINE + 0x20000)
#define ANE_TQ			(ANE_ENGINE + 0x21000)

/* Task-manager registers (offsets from ANE_TM) */
#define TM_ADDR			0x00
#define TM_INFO			0x04
#define TM_PUSH			0x08
#define TM_TQ_EN		0x0c
#define TM_IRQ_EVTC(l)		(0x14 + (l) * 0x14)
#define TM_IRQ_INFO(l)		(0x18 + (l) * 0x14)
#define TM_IRQ_UNK1(l)		(0x1c + (l) * 0x14)
#define TM_IRQ_TMST(l)		(0x20 + (l) * 0x14)
#define TM_IRQ_UNK2(l)		(0x24 + (l) * 0x14)
#define TM_COMMITTED		0x44
#define TM_STATUS		0x54
#define TM_ERROR1		0x58
#define TM_ERROR2		0x5c
#define TM_ERROR3		0x60
#define TM_IRQ_EN1		0x68
#define TM_IRQ_ACK		0x6c
#define TM_IRQ_EN2		0x70

#define TM_IS_IDLE		0x1

/* Task-queue registers (offsets from ANE_TQ; per-queue stride 0x148) */
#define TQ_STRIDE		0x148
#define TQ_STATUS(q)		((q) * TQ_STRIDE + 0x000)
#define TQ_PRTY(q)		((q) * TQ_STRIDE + 0x010)
#define TQ_BAR1(q, b)		((q) * TQ_STRIDE + 0x020 + (b) * 4)
#define TQ_NID1(q)		((q) * TQ_STRIDE + 0x0a0)
#define TQ_SIZE1(q)		((q) * TQ_STRIDE + 0x130)
#define TQ_ADDR1(q)		((q) * TQ_STRIDE + 0x134)

#define ANE_TQ_COUNT		8
#define ANE_TILE_COUNT		0x20
static const unsigned int TQ_PRTY_TABLE[ANE_TQ_COUNT] = {
	0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x1e, 0x1f,
};

/* Submit constants (ane_drv.c ane_submit) */
#define QID			4
#define ANE_FIFO_NID		0x40
#define CMD_BUF_BDX		0
#define KRN_BUF_BDX		1
#define ANE_CMD_GRAN		0x10
#define OUT_BDX			4	/* output channel 4   */
#define IN0_BDX			5	/* input channel 5    */
#define IN1_BDX			6	/* input channel 6    */

/* DART (Gen1 / t8020 layout), three instances for this engine */
#define DART0			0x26b800000UL
#define DART1			0x26b810000UL
#define DART2			0x26b820000UL

#define DART_STREAM_COMMAND	0x20
#define DART_STREAM_CMD_BUSY	(1U << 2)
#define DART_STREAM_CMD_INVAL	(1U << 20)
#define DART_STREAM_SELECT	0x34
#define DART_ERROR		0x40
#define DART_ERROR_FLAG		(1U << 31)
#define DART_ERROR_ADDR_LO	0x50
#define DART_ERROR_ADDR_HI	0x54
#define DART_STREAMS_ENABLE	0xfc
#define DART_TCR		0x100		/* + sid*4 */
#define DART_TCR_TRANSLATE	(1U << 7)
#define DART_TTBR		0x200		/* + (4*sid + idx)*4 */
#define DART_TTBR_VALID		(1U << 31)
#define DART_TTBR_SHIFT		12

/* Gen1 (APPLE_DART) pte: u64; paddr in [35:12]; SUBPAGE_START
 * [63:52]=0, SUBPAGE_END [51:40]=0xfff for a full 4K page; VALID bit0;
 * prot bits NO_READ=bit8, NO_WRITE=bit7 left clear for RW. */
#define DART_PTE_VALID		(1ULL << 0)
#define DART_PTE_SUBPAGE_END	(0xfffULL << 40)
#define DART_PTE_ADDR_MASK	0x0000000ffffff000ULL

/* s5l uart (apple,s5l-uart): TX path only */
#define UART_UTRSTAT		0x10		/* bit1 = TX empty      */
#define UART_UTRSTAT_TXE	(1U << 1)
#define UART_UTXH		0x20

#endif /* ANE_ADD_REGS_H */
