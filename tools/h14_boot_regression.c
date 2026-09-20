/* SPDX-License-Identifier: GPL-2.0-only OR MIT
 *
 * Offline regression for the W15 boot contract units
 * (ane/t6021/ane_t6021_boot.h) — userspace twin of the kernel
 * consumers (ane_t6021_boot.c sequencer + the pending publication
 * increment). Shipped in this repository so the tested unit and its
 * check travel together.
 *
 * Build:
 *   gcc -Wall -Wextra -O2 -I ane/t6021 \
 *       -o h14_boot_regression tools/h14_boot_regression.c
 * Exit 0 = every positive and negative case behaved as asserted.
 *
 * Byte anchors: KC 8304156f… (ANE_Init 0x95e9850–0x95e9988 RVBAR fold,
 * 0x95ea710–0x95ea97c init suballocation), selene 9f7915c4…; receipts
 * 2026-09-20-h14-rvbar-width / -legacy-init-publication /
 * mapper-callchain-audit (commits 3762aee, 12be074, 04630ef, c364f24).
 */
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

#include "ane_t6021_boot.h"

static int failures;
static int checks;

static void check(int cond, const char *name, const char *detail)
{
	checks++;
	if (cond) {
		printf("PASS %-44s (%s)\n", name, detail);
	} else {
		failures++;
		printf("FAIL %-44s (%s)\n", name, detail);
	}
}

static u64 rd_le64(const u8 *p)
{
	u64 v = 0;
	int i;

	for (i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

static u32 rd_le32(const u8 *p)
{
	return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) |
	       ((u32)p[3] << 24);
}


/* ---- fake MMIO backend (test-only; the kernel wraps real io in
 * ane_t6021_boot.c at gate-flip time) ---- */
struct fake {
	unsigned int woff[64];
	u32 wval[64];
	int nwr;
	int nw64;
	u64 w64val;
	int prepare_at;
	int cpuctrl0_at;
	int wake_at;
	int barrier_at;
	u64 rvbar;
	u32 s7[4];
	int n7;
	int polls;
	int s7_never_ack;
	int failed;
	u32 prep_lo;
	u32 prep_hi;
};

static struct fake *fake;

static void fake_reset(struct fake *f)
{
	memset(f, 0, sizeof(*f));
	f->nw64 = -1;
	f->prepare_at = -1;
	f->cpuctrl0_at = -1;
	f->wake_at = -1;
	f->barrier_at = -1;
}

static u32 f_rd32(void *ctx, unsigned int off)
{
	struct fake *f = ctx ? ctx : fake;
	(void)off;
	if (f->s7_never_ack)
		return 0;
	if (f->n7 < (int)(sizeof(f->s7) / sizeof(f->s7[0])))
		return f->s7[f->n7++];
	return ANE_T6021_BOOT_ACK;
}

static u64 f_rd64(void *ctx, unsigned int off)
{
	struct fake *f = ctx ? ctx : fake;
	(void)off;
	return f->rvbar;
}

static void f_rec(struct fake *f, unsigned int off, u32 v)
{
	if (f->nwr < (int)(sizeof(f->woff) / sizeof(f->woff[0]))) {
		f->woff[f->nwr] = off;
		f->wval[f->nwr] = v;
	}
	f->nwr++;
}

static void f_wr32(void *ctx, unsigned int off, u32 v)
{
	struct fake *f = fake;

	(void)ctx;

	if (off == ANE_T6021_BOOT_REG_CPUCTRL && v == 0)
		f->cpuctrl0_at = f->nwr;
	if (off == ANE_T6021_BOOT_REG_SCRATCH7 && v == ANE_T6021_BOOT_WAKE_REQ)
		f->wake_at = f->nwr;
	f_rec(f, off, v);
}

static void f_wr64(void *ctx, unsigned int off, u64 v)
{
	struct fake *f = fake;

	(void)ctx;

	(void)off;
	f->nw64 = f->nwr;
	f->w64val = v;
	f_rec(f, off, (u32)v);
}

static void f_dsb(void *ctx)
{
	struct fake *f = fake;

	(void)ctx;
	f->barrier_at = f->nwr;
}

static void f_wait(void *ctx)
{
	struct fake *f = fake;

	(void)ctx;
	f->polls++;
}

static int f_prepare(void *ctx, u32 *lo, u32 *hi)
{
	struct fake *f = fake;

	(void)ctx;
	f->prepare_at = f->nwr;
	f->prep_lo = 0x1111;
	f->prep_hi = 0x2222;
	*lo = f->prep_lo;
	*hi = f->prep_hi;
	return 0;
}

int main(void)
{
	/* ---- RVBAR entry fold (ANE_Init 0x95e9868-0x95e988c algebra:
	 * ENTRY_BASE | (iova & ADDR_MASK); mask clears bits 0-10, 48,
	 * 55 and RETAINS bit 11; base supplies bit0 + fixed upper
	 * pattern). Vectors computed independently of the header
	 * implementation. ---- */
	check(ane_t6021_rvbar_compose(0) == 0x0081000000000001ULL,
	      "compose(0)", "pure valid bit, entry 0");
	check(ane_t6021_rvbar_compose(0x1000) == 0x0081000000001001ULL,
	      "compose(0x1000)", "aligned pass-through");
	check(ane_t6021_rvbar_compose(0x500000) == 0x0081000000500001ULL,
	      "compose(0x500000)", "FWIM surface size (config+0x138)");
	check(ane_t6021_rvbar_compose(0x123456789ULL) ==
	      0x0081000123456001ULL,
	      "compose(0x123456789)", "bits 0-10 dropped");
	check(ane_t6021_rvbar_compose(0xdeadbeef000ULL) ==
	      0x00810deadbeef001ULL,
	      "compose(0xdeadbeef000)", "mid-range DVA");
	check(ane_t6021_rvbar_compose(0x7f123456789ULL) ==
	      0x008107f123456001ULL,
	      "compose(0x7f123456789)", "39-bit DVA");
	check(ane_t6021_rvbar_compose(0x0001800000001000ULL) ==
	      0x0081800000001001ULL,
	      "compose bit48+55 cleared, bit47 survives",
	      "byte(48-55)=0x01 masked by 0x7e; byte(40-47)=0x80 kept");
	check(ane_t6021_rvbar_compose(0x0100800000001000ULL) ==
	      0x0181800000001001ULL,
	      "compose bit47+56 survive",
	      "bit47 in byte5 and bit56 in byte7 are outside the clears");

	/* ---- Latch/entry decode (tbnz w0,#0 skip, ANE_Init
	 * 0x95e9878; live W8/W10 reads 0x1). ---- */
	check(ane_t6021_rvbar_latched(0x1), "latched(W8/W10 live 0x1)",
	      "bit0 set");
	check(ane_t6021_rvbar_latched(0) == false, "latched(0)",
	      "bit0 clear = unprogrammed");
	check(ane_t6021_rvbar_latched(0x0081000000001001),
	      "latched(post-write readback)", "fold sets bit0");
	check(ane_t6021_rvbar_entry_bits(0x1) == 0,
	      "entry_bits(live 0x1) == 0", "entry field zero");
	check(ane_t6021_rvbar_entry_bits(0x00810deadbeef001) ==
	      0xdeadbeef000,
	      "entry_bits round-trip", "entry recoverable from a read");
	check(ane_t6021_rvbar_entry_bits(0x0081000123456001) ==
	      0x123456000,
	      "entry_bits low-bit truncation", "matches the fold algebra");

	/* ---- Acceptance predicate — the EXACT check the boot path
	 * applies (ane_t6021_rvbar_entry_ok): accept clean iovas and
	 * bit-11 iovas (retained), reject any iova carrying a dropped
	 * bit (0-10, 48, 55). ---- */
	{
		static const struct {
			u64 iova;
			bool ok;
			const char *why;
		} rt[] = {
			{ 0xdeadbeef000ULL, true, "clean DVA accepted" },
			{ 0x800ULL, true, "bit 11 RETAINED — accepted" },
			{ 0x400ULL, false, "bit 10 rejected" },
			{ 0x200ULL, false, "bit 9 rejected" },
			{ 0x7ffULL, false, "entire low mask 0-10 rejected" },
			{ 0x0001000000000000ULL, false,
			  "bit 48 rejected" },
			{ 0x0080000000000000ULL, false,
			  "bit 55 rejected" },
			{ 0x0080000000001800ULL, false,
			  "bit 55 rejected even with retained bits 11+12" },
		};
		unsigned int i;

		for (i = 0; i < sizeof(rt) / sizeof(rt[0]); i++) {
			u64 got = ane_t6021_rvbar_entry_bits(
				ane_t6021_rvbar_compose(rt[i].iova));

			check(ane_t6021_rvbar_entry_ok(rt[i].iova) ==
			      rt[i].ok, "entry acceptance", rt[i].why);
			/* the fold agrees with the predicate: accepted
			 * iovas round-trip exactly, rejected ones lose
			 * bits */
			check(got == (rt[i].iova &
				      ANE_T6021_RVBAR_ADDR_MASK),
			      "fold algebra", rt[i].why);
		}
	}

	/* ---- SCRATCH0/1 u64 split (publication convention: low32 ->
	 * SCRATCH0 0x01840048 first, high32 -> SCRATCH1 0x0184004c,
	 * dsb st before; legacy-init-publication receipt). ---- */
	{
		u32 lo = 0, hi = 0;

		ane_t6021_scratch64_split(0x123456789abcdef0ULL, &lo, &hi);
		check(lo == 0x9abcdef0U && hi == 0x12345678U,
		      "split low/high halves", "lo first convention");
		check(ane_t6021_scratch64_join(lo, hi) ==
		      0x123456789abcdef0ULL,
		      "join(split(v)) == v", "u64 reassembly");
		check(ane_t6021_scratch64_join(0xffffffffU, 0) ==
		      0x00000000ffffffffULL,
		      "join zero-extends hi", "no sign extension");
	}

	/* ---- Init suballocation SOURCED fill (pass5b-5d/6c corrected
	 * contract; validator check_init_contract.py 68/68): EVERY fw-read
	 * field is sourced — [0x00] FWIM DVA, [0x08] 'IPC ' DVA (size =
	 * max(u64[dev+0x3A90], zext(u32[x23+4]))), [0x10] config+0x138 =
	 * 0x500000, [0x18] = 0x10000000 - size = 0x0fb00000, [0x20] obj2
	 * DVA, [0x28] w22, [0x30] fw-load progress, [0x50] zext [x23+4],
	 * [0x58] pool DMA base, [0x60] pool word0, [0x68] count 64,
	 * template[0x00] = 0, template+0xC0 = 4. The preflight gate keeps
	 * the sequence closed until every one of these values is pinned. */
	{
		static const struct ane_t6021_init_sources src = {
			.fw_dva = 0x0000deadbeef000ULL,
			.dev_3a90 = 0x12340000ULL,
			.x23p4 = 0x2000,
			.obj2_dva = 0x0000feedface000ULL,
			.w22 = 0x2a,
			.fwload_progress = 0x11,
			.cfg_size = 0x500000,
			.pool_dma = 0x5555aaaab000ULL,
		};
		u8 buf[ANE_T6021_INIT_STRUCT_SIZE];
		u8 leak[ANE_T6021_INIT_STRUCT_SIZE];
		static const u8 golden[ANE_T6021_INIT_STRUCT_SIZE] = {
		0x00, 0xf0, 0xee, 0xdb, 0xea, 0x0d, 0x00, 0x00, 0x00, 0x00, 0x34, 0x12, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb0, 0x0f, 0x00, 0x00, 0x00, 0x00,
		0x00, 0xe0, 0xac, 0xdf, 0xee, 0x0f, 0x00, 0x00, 0x2a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb0, 0xaa, 0xaa, 0x55, 0x55, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		};
		size_t i;
		int touched;

		check(ANE_T6021_INIT_COUNT_OFF + 4 +
			      ANE_T6021_INIT_TEMPLATE_SIZE == 0x16c,
		      "template end 0x16c", "0x68+4+0x100");
		check(0x16c + 8 == ANE_T6021_INIT_STRUCT_SIZE,
		      "struct size 0x174", "0x16c + header qword");

		memset(buf, 0, sizeof(buf));
		ane_t6021_init_struct_fill(buf, &src);
		check(memcmp(buf, golden, sizeof(buf)) == 0,
		      "sourced fill exact image",
		      "every fw-read field sourced, gaps zero");

		check(rd_le64(buf) == src.fw_dva &&
		      rd_le64(buf + 0x08) ==
		      ane_t6021_ipc_size(0x12340000ULL, 0x2000) &&
		      rd_le64(buf + 0x08) == 0x12340000ULL &&
		      rd_le64(buf + 0x10) == 0x500000ULL &&
		      rd_le64(buf + 0x18) == 0x0fb00000ULL &&
		      rd_le32(buf + 0x28) == 0x2a &&
		      rd_le32(buf + 0x30) == 0x11 &&
		      rd_le64(buf + 0x50) == 0x2000ULL &&
		      rd_le32(buf + ANE_T6021_INIT_COUNT_OFF) == 0x40 &&
		      rd_le32(buf + ANE_T6021_INIT_TEMPLATE_OFF +
			      ANE_T6021_INIT_TBIT_OFF) == 0x4,
		      "fill field reads", "all sourced fields decoded");

		/* untouched-zone check: fill writes only its closed fields —
		 * the gaps [0x34..37]/[0x64..67] stay caller-owned */
		memset(leak, 0xa5, sizeof(leak));
		ane_t6021_init_struct_fill(leak, &src);
		touched = 0;
		for (i = 0; i < sizeof(leak); i++) {
			int closed = i < 0x34 ||
				     (i >= 0x50 && i < 0x70) ||
				     i == (ANE_T6021_INIT_TEMPLATE_OFF +
					   ANE_T6021_INIT_TBIT_OFF);

			if (!closed && leak[i] != 0xa5)
				touched++;
		}
		check(touched == 0, "fill touches sourced fields only",
		      "gap bytes preserved byte-exact (zero first)");
	}


	/* ---- Fake-MMIO trace check: ane_t6021_boot_run against the
	 * recording backend. Asserts (1) ZERO writes while the preflight
	 * gate is closed, (2) exact write order/values on the bit0-set
	 * skip branch, (3) the fold write on the unprogrammed branch,
	 * (4) publish strictly after poll A, wake strictly after
	 * publish, (5) poll A timeout = hold + DMA not reclaimable. ---- */
	{
		struct fake fk;
		static const struct ane_t6021_boot_io io = {
			.rd32 = f_rd32, .rd64 = f_rd64,
			.wr32 = f_wr32, .wr64 = f_wr64,
			.dsb_st = f_dsb, .poll_wait = f_wait,
			.prepare = f_prepare,
		};

		/* (1) gates closed -> -ENODATA and NOT ONE write */
		fake_reset(&fk);
		fake = &fk;
		{
			int cs = 0, fa = 0, bo = 0;
			struct ane_t6021_boot_cfg cfg = {
				.preflight_ok = 0,
				.fw_dva = 0x500000,
			};

			check(ane_t6021_boot_run(&io, &cfg, &cs, &fa,
						 &bo) == -ENODATA,
			      "run: closed gate returns -ENODATA",
			      "no sequence started");
			check(fk.nwr == 0, "run: closed gate = no writes",
			      "Main: all gates before any boot write");
			check(cs == 0 && fa == 0 && bo == 0,
			      "run: closed gate sets no flags",
			      "state stays fenced");
		}

		/* (2) open gate, bit0-set RVBAR (live state): lawful
		 * skip branch. */
		fake_reset(&fk);
		fk.rvbar = 0x1;
		{
			int cs = 0, fa = 0, bo = 0;
			struct ane_t6021_boot_cfg cfg = {
				.preflight_ok = 1,
				.fw_dva = 0x0000deadbeef000ULL,
			};
			int r = ane_t6021_boot_run(&io, &cfg, &cs, &fa,
						   &bo);

			check(r == 0, "run: happy path returns 0",
			      "fresh READY and DONE both observed");
			check(cs == 1 && fa == 1 && bo == 1,
			      "run: cpu_started/fw_alive/booted set",
			      "full sequence");
			check(fk.nwr == 19,
			      "run: 19 writes (3 table + 8 clear + s6 + pulse2 + cpu2 + pub2 + wake)",
			      "exact write count");
			check(fk.woff[0] == ANE_T6021_BOOT_REG_TABLE0 &&
			      fk.woff[1] == ANE_T6021_BOOT_REG_TABLE1 &&
			      fk.woff[2] == ANE_T6021_BOOT_REG_TABLE2 &&
			      fk.wval[0] == 0x01ff01ffU &&
			      fk.wval[1] == 0x01ff01ffU &&
			      fk.wval[2] == 0x01ff01ffU,
			      "pre-CPU table writes first",
			      "pass4 receiver, pass5 gate");
			check(fk.woff[3] == ANE_T6021_BOOT_REG_SCRATCH0 &&
			      fk.wval[3] == 0 &&
			      fk.woff[10] ==
			      ANE_T6021_BOOT_REG_SCRATCH7 &&
			      fk.wval[10] == 0,
			      "all eight scratch cells cleared",
			      "exact init clears ALL, not only 7");
			check(fk.woff[11] ==
			      ANE_T6021_BOOT_REG_SCRATCH6 &&
			      fk.wval[11] == 1 &&
			      fk.woff[12] ==
			      ANE_T6021_BOOT_REG_SCRATCH7 &&
			      fk.wval[12] == 1 &&
			      fk.woff[13] ==
			      ANE_T6021_BOOT_REG_SCRATCH7 &&
			      fk.wval[13] == 0,
			      "SCRATCH6=1 then SCRATCH7 pulse 1->0",
			      "stale ack cleared pre-CPU");
			check(fk.nw64 < 0,
			      "no RVBAR write on bit0-set branch",
			      "lawful skip; never write over bit0");
			check(fk.cpuctrl0_at == 14 &&
			      fk.wval[14] == 0 &&
			      fk.wval[15] == ANE_T6021_CPU_RUN_RELEASE,
			      "CPU_CONTROL 0 then 0x10 after pulse",
			      "strict order, both paths");
			check(fk.prepare_at == 16 &&
			      fk.barrier_at == 16,
			      "prepare+dsb after poll A",
			      "publication strictly post-alive");
			check(fk.woff[16] ==
			      ANE_T6021_BOOT_REG_SCRATCH0 &&
			      fk.wval[16] == 0x1111U &&
			      fk.woff[17] ==
			      ANE_T6021_BOOT_REG_SCRATCH1 &&
			      fk.wval[17] == 0x2222U,
			      "publish low32/high32 from prepare",
			      "suballoc DVA halves");
			check(fk.woff[18] ==
			      ANE_T6021_BOOT_REG_SCRATCH7 &&
			      fk.wval[18] == ANE_T6021_BOOT_WAKE_REQ,
			      "wake after publish", "releases fw wait");
		}

		/* (3) bit0-clear RVBAR: the fold write64 appears */
		fake_reset(&fk);
		{
			int cs = 0, fa = 0, bo = 0;
			struct ane_t6021_boot_cfg cfg = {
				.preflight_ok = 1,
				.fw_dva = 0x0000deadbeef000ULL,
			};

			check(ane_t6021_boot_run(&io, &cfg, &cs, &fa,
						 &bo) == 0,
			      "run: unprogrammed branch boots",
			      "direct-image fold path");
			check(fk.nw64 == 14 &&
			      fk.w64val ==
			      ane_t6021_rvbar_compose(cfg.fw_dva),
			      "RVBAR write64 = entry fold",
			      "after table+scratch pulse, before CPU release");
		}

		/* (4) poll A timeout: HOLD — started, nothing published */
		fake_reset(&fk);
		fk.s7_never_ack = 1;
		{
			int cs = 0, fa = 0, bo = 0;
			struct ane_t6021_boot_cfg cfg = {
				.preflight_ok = 1,
				.fw_dva = 0x0000deadbeef000ULL,
			};

			check(ane_t6021_boot_run(&io, &cfg, &cs, &fa,
						 &bo) == -ETIMEDOUT,
			      "run: poll A timeout = -ETIMEDOUT",
			      "bounded polls");
			check(cs == 1 && fa == 0 && bo == 0,
			      "timeout: cpu_started, not alive/booted",
			      "HOLD semantics");
			check(fk.prepare_at < 0,
			      "timeout: no publish attempted",
			      "no partial boot past poll A");
			check(fk.nwr == 17,
			      "timeout: writes stop after CPU release (17 incl. fold wr64)",
			      "no publish/wake after poll A timeout");
			check(ane_t6021_boot_dma_reclaimable(cs) == false,
			      "ownership: DMA NOT reclaimable while started",
			      "cannot free while the CPU may fetch");
		}
		check(ane_t6021_boot_dma_reclaimable(0) == true,
		      "ownership: DMA reclaimable when never started",
		      "normal status-only removal");
		check(ane_t6021_boot_remove_held(1) == true &&
		      ane_t6021_boot_remove_held(0) == false,
		      "remove-held predicate",
		      "whole lifetime held while started (H13 wedge)");
		fake = NULL;
	}

	printf("%s: %d checks, %d failures\n",
	       failures ? "FAILED" : "PASSED", checks, failures);
	return failures != 0;
}
