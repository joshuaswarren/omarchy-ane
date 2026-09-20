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

/* Linux sources for the init-suballocation fields (pass5b/5c/5d
 * b8b7c35 + pass6c 75a017b; validator tools/check_init_contract.py
 * 68/68). EVERY fw-read field is explicitly sourced from the struct
 * below — nothing is silently zero: [0x00] FWIM DVA, [0x08] 'IPC '
 * surface DVA (Linux alloc, size = ane_t6021_ipc_size), [0x10]
 * config+0x138 = 0x500000, [0x18] = 0x10000000 - size, [0x20] obj2
 * DVA, [0x28] w22, [0x30] fw-load progress, [0x50] zext [x23+4],
 * [0x58] pool DMA base, [0x60] pool word0, [0x68] count 64,
 * template[0x00] = 0, template+0xC0 = 4. The struct values pin BEFORE
 * the preflight gate can open (Main: no unresolved field silently
 * zero; no partial boot). The caller MUST zero the full 0x174 block
 * first (coherent alloc): gaps [0x34..37]/[0x64..67] get no kext
 * store and sit inside fw-read u64 units. */
/* IPC surface size (pass6c 75a017b): max of the config-side u64 field
 * dev+0x3A90 (single writer start+0x28a8) and the zero-extended u32
 * [x23+4]; both operands pin before the preflight can open. */
static inline u64 ane_t6021_ipc_size(u64 dev_3a90, u32 x23p4)
{
	u64 big = x23p4;

	return dev_3a90 > big ? dev_3a90 : big;
}

struct ane_t6021_init_sources {
	u64 fw_dva;		/* [0x00] staged selene ('FWIM') DVA */
	u64 ipc_dva;		/* [0x08] 'IPC ' surface DVA (Linux alloc) */
	u32 cfg_size;		/* [0x10] config+0x138 = 0x500000 */
	u64 obj2_dva;		/* [0x20] *(obj2+0x18) — pinned at close */
	u32 w22;		/* [0x28] ANE_Init local — pinned at close */
	u32 fwload_progress;	/* [0x30] dev+0x990 — pinned at close */
	u32 x23p4;		/* [0x50] zext u32 [x23+4] */
	u64 dev_3a90;		/* [0x50]-pair + IPC size operand */
	u64 pool_dma;		/* [0x58] init-pool DMA base */
	u64 pool_word0;		/* [0x60] pool first qword (write 0) */
};

static inline void
ane_t6021_init_struct_fill(u8 *buf, const struct ane_t6021_init_sources *s)
{
	u64 size = s->cfg_size;
	u64 ipc = ane_t6021_ipc_size(s->dev_3a90, s->x23p4);
	int i;

	for (i = 0; i < 8; i++) {
		buf[ANE_T6021_INIT_FW_DVA_OFF + i] =
			(u8)(s->fw_dva >> (8 * i));
		buf[0x08 + i] = (u8)(ipc >> (8 * i));
		buf[0x10 + i] = (u8)(size >> (8 * i));
		buf[0x18 + i] = (u8)((0x10000000ULL - size) >> (8 * i));
		buf[0x20 + i] = (u8)(s->obj2_dva >> (8 * i));
		buf[0x50 + i] = (u8)((u64)s->x23p4 >> (8 * i));
		buf[0x58 + i] = (u8)(s->pool_dma >> (8 * i));
		buf[0x60 + i] = (u8)(s->pool_word0 >> (8 * i));
	}

	buf[0x28] = (u8)s->w22;
	buf[0x29] = 0;
	buf[0x2a] = 0;
	buf[0x2b] = 0;
	buf[0x30] = (u8)s->fwload_progress;
	buf[0x31] = 0;
	buf[0x32] = 0;
	buf[0x33] = 0;

	buf[ANE_T6021_INIT_COUNT_OFF + 0] = (u8)ANE_T6021_INIT_COUNT;
	buf[ANE_T6021_INIT_COUNT_OFF + 1] = 0;
	buf[ANE_T6021_INIT_COUNT_OFF + 2] = 0;
	buf[ANE_T6021_INIT_COUNT_OFF + 3] = 0;

	/* template[0x00] = config word [dev+0x1D8], pinned value 0
	 * (pass5d); [0x04..0x0B] zero via the caller's zeroing. */
	buf[ANE_T6021_INIT_TEMPLATE_OFF + 0] = 0;
	buf[ANE_T6021_INIT_TEMPLATE_OFF + 1] = 0;
	buf[ANE_T6021_INIT_TEMPLATE_OFF + 2] = 0;
	buf[ANE_T6021_INIT_TEMPLATE_OFF + 3] = 0;

	/* template+0xC0 = 4: RESOLVED initial state (Main raw anchors
	 * 0x9612b78/7c/80). The conditional |=0x10 remains unset. */
	buf[ANE_T6021_INIT_TEMPLATE_OFF + ANE_T6021_INIT_TBIT_OFF + 0] =
		ANE_T6021_INIT_TBIT_VAL;
}

/* ---- Fake-MMIO-testable sequence core ----
 *
 * Register offsets (ENGINE-relative; the Linux driver maps the engine
 * window, so kernel offsets == trace offsets) and the io backend.
 * ane_t6021_boot_run() executes the resolved total order against ANY
 * backend: the kernel wraps readl/writeq/udelay; the host regression
 * wraps a recording fake (trace asserts: full order, no writes when
 * the preflight gate is closed, publish strictly after poll A). */

#define ANE_T6021_BOOT_REG_TABLE0	0x00000b38
#define ANE_T6021_BOOT_REG_TABLE1	0x00000b98
#define ANE_T6021_BOOT_REG_TABLE2	0x00000bf8
#define ANE_T6021_BOOT_REG_RVBAR	0x01050000
#define ANE_T6021_BOOT_REG_CPUCTRL	0x01400044
#define ANE_T6021_BOOT_REG_SCRATCH0	0x01840048
#define ANE_T6021_BOOT_REG_SCRATCH1	0x0184004c
#define ANE_T6021_BOOT_REG_SCRATCH6	0x01840060
#define ANE_T6021_BOOT_REG_SCRATCH7	0x01840064
#define ANE_T6021_BOOT_TABLE_VALUE	0x01ff01ffU
#define ANE_T6021_BOOT_TABLE_POLLS	1000

struct ane_t6021_boot_io {
	void *ctx;
	u32 (*rd32)(void *ctx, unsigned int off);
	u64 (*rd64)(void *ctx, unsigned int off);
	void (*wr32)(void *ctx, unsigned int off, u32 v);
	void (*wr64)(void *ctx, unsigned int off, u64 v);
	void (*dsb_st)(void *ctx);       /* dsb st equivalent */
	void (*poll_wait)(void *ctx);    /* 1 ms poll delay */
	/* prepare(): called once, strictly AFTER poll A and BEFORE the
	 * SCRATCH0/1 publish; returns the suballoc DVA halves. Kernel
	 * backend: allocate pool/IPC + fill from pinned sources. */
	int (*prepare)(void *ctx, u32 *lo, u32 *hi);
};

struct ane_t6021_boot_cfg {
	int preflight_ok;	/* EVERY prerequisite closed (Main: no
				 * partial boot — the whole sequence or
				 * nothing) */
	u64 fw_dva;		/* staged surface DVA (fold input) */
};

/* Ownership: a started CPU may be fetching from the staged surfaces —
 * the DMA memory is NOT reclaimable on failure/remove while
 * cpu_started; reclaimable only via the domain-off reset (reboot). */
static inline int ane_t6021_boot_dma_reclaimable(int cpu_started)
{
	return !cpu_started;
}

/* Returns 0 (DONE observed, booted), -ENODATA (preflight closed: ZERO
 * io writes), or -ETIMEDOUT (poll A/B: cpu_started holds, DMA stays
 * unreclaimable, no publish/wake happened on poll A timeout). */
static inline int
ane_t6021_boot_run(const struct ane_t6021_boot_io *io,
		   const struct ane_t6021_boot_cfg *cfg,
		   int *cpu_started, int *fw_alive, int *booted)
{
	unsigned int i;
	u32 v;
	u64 rvbar;

	*cpu_started = 0;
	*fw_alive = 0;
	*booted = 0;

	/* Main: all gates checked BEFORE any boot write — no partial
	 * sequence when the preflight is closed. */
	if (!cfg->preflight_ok)
		return -ENODATA;

	/* pre-CPU engine table (pass4 receiver, pass5 gate: REQUIRED
	 * every EnableANEClocksAndPower; residual alias/indirect
	 * writer risk documented in the preflight list). */
	io->wr32(io->ctx, ANE_T6021_BOOT_REG_TABLE0,
		 ANE_T6021_BOOT_TABLE_VALUE);
	io->wr32(io->ctx, ANE_T6021_BOOT_REG_TABLE1,
		 ANE_T6021_BOOT_TABLE_VALUE);
	io->wr32(io->ctx, ANE_T6021_BOOT_REG_TABLE2,
		 ANE_T6021_BOOT_TABLE_VALUE);

	/* S1: InitANEScratchRegisters — clear ALL cells, SCRATCH6 = 1,
	 * pulse SCRATCH7 1 -> 0 (stale READY/wake cleared pre-CPU). */
	for (i = 0; i < 8; i++)
		io->wr32(io->ctx,
			 ANE_T6021_BOOT_REG_SCRATCH0 + 4 * i, 0);
	io->wr32(io->ctx, ANE_T6021_BOOT_REG_SCRATCH6, 1);
	io->wr32(io->ctx, ANE_T6021_BOOT_REG_SCRATCH7, 1);
	io->wr32(io->ctx, ANE_T6021_BOOT_REG_SCRATCH7, 0);

	/* S2: RVBAR skip-or-fold (bit0 set = lawful skip branch; no
	 * latch override, no reset before first attempt). */
	rvbar = io->rd64(io->ctx, ANE_T6021_BOOT_REG_RVBAR);
	if (!ane_t6021_rvbar_latched(rvbar))
		io->wr64(io->ctx, ANE_T6021_BOOT_REG_RVBAR,
			 ane_t6021_rvbar_compose(cfg->fw_dva));

	/* S3: CPU release — both paths, strictly 0 then 0x10. */
	io->wr32(io->ctx, ANE_T6021_BOOT_REG_CPUCTRL, 0);
	io->wr32(io->ctx, ANE_T6021_BOOT_REG_CPUCTRL,
		 ANE_T6021_CPU_RUN_RELEASE);
	*cpu_started = 1;

	/* S4: poll A — FRESH READY (the pulse made it unambiguous). */
	for (i = 0; i < ANE_T6021_BOOT_TABLE_POLLS; i++) {
		v = io->rd32(io->ctx, ANE_T6021_BOOT_REG_SCRATCH7);
		if (v == ANE_T6021_BOOT_ACK)
			break;
		io->poll_wait(io->ctx);
	}
	if (v != ANE_T6021_BOOT_ACK)
		return -ETIMEDOUT;
	*fw_alive = 1;

	/* S5-S6: prepare (alloc/fill; kernel backend owns DMA) then
	 * dsb st + publish suballoc DVA low32/high32, then wake. */
	{
		u32 lo = 0, hi = 0;
		int err = io->prepare(io->ctx, &lo, &hi);

		if (err)
			return err;
		io->dsb_st(io->ctx);
		io->wr32(io->ctx, ANE_T6021_BOOT_REG_SCRATCH0, lo);
		io->wr32(io->ctx, ANE_T6021_BOOT_REG_SCRATCH1, hi);
	}
	io->wr32(io->ctx, ANE_T6021_BOOT_REG_SCRATCH7,
		 ANE_T6021_BOOT_WAKE_REQ);

	/* S7: poll B — DONE; read back the SCRATCH0/1 result u64. */
	for (i = 0; i < ANE_T6021_BOOT_TABLE_POLLS; i++) {
		v = io->rd32(io->ctx, ANE_T6021_BOOT_REG_SCRATCH7);
		if (v == ANE_T6021_BOOT_ACK)
			break;
		io->poll_wait(io->ctx);
	}
	if (v != ANE_T6021_BOOT_ACK)
		return -ETIMEDOUT;
	*booted = 1;
	io->rd32(io->ctx, ANE_T6021_BOOT_REG_SCRATCH1);
	io->rd32(io->ctx, ANE_T6021_BOOT_REG_SCRATCH0);
	return 0;
}

#endif /* __ANE_T6021_BOOT_H__ */
