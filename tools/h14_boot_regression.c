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
#include <string.h>
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
	u32 scratch0_val;
	u32 scratch1_val;
	int polls;
	int s7_never_ack;
	int failed;
	int fail_alloc_idx;   /* fail the Nth allocation (-1 = none) */
	const char *phase[16];
	int nphase;
	u32 prep_lo;
	u32 prep_hi;
	u32 scratch3_req;
	u32 scratch1_captured;
	u64 asz[8];
	u64 aiov[8];
	int nasz;
};

/* file-scope sources for the shared assembly (nonzero prev_fw_len on
 * purpose: catches low-byte-only serialization) */
static const struct ane_t6021_init_sources asm_src = {
	.fw_dva = 0x0000deadbeef000ULL,
	.ipc_dva = 0,   /* ZERO input: [0x08] must come from the
			 * ALLOCATOR output (Main zero-DVA bug) */
	.cfg_size = 0x500000,
	.prev_fw_len = 0x1234,
	.heap_floor = 0x30000ULL,
	.pool_dma = 0,  /* ZERO input: [0x58] must come from the
			 * ALLOCATOR output */
	.pool_word0 = 0x40000, /* pool total length: HYPOTHESIS-GRADE in
				* shipped sources; here it exercises the
				* fill's u64 copy — the SHIPPED value
				* stays hard-gated */
};

static struct fake *fake;
static u8 fake_mem[0x80000];
static size_t fake_mem_off;

static void *f_alloc2(void *ctx, u64 size, u64 *iova)
{
	struct fake *f = fake;
	void *p;

	(void)ctx;
	if (f->nasz == f->fail_alloc_idx || f->nasz >= 8) {
		*iova = 0;
		return NULL;
	}
	p = fake_mem + fake_mem_off;
	/* call-order iovas: pool, 'IPC ', HEAP — matching the golden */
	switch (f->nasz) {
	case 0: *iova = 0x5555aaaab000ULL; break;
	case 1: *iova = 0x00000badc0de000ULL; break;
	case 2: *iova = 0x0000feedface000ULL; break;
	default: *iova = 0x10000000ULL * (u64)(f->nasz + 1); break;
	}
	f->asz[f->nasz] = size;
	f->aiov[f->nasz] = *iova;
	f->nasz++;
	fake_mem_off = (fake_mem_off + size + 0xfffULL) & ~(size_t)0xfffULL;
	return p;
}

static struct fake *fake;

static void fake_reset(struct fake *f)
{
	memset(f, 0, sizeof(*f));
	fake_mem_off = 0;
	f->nw64 = -1;
	f->prepare_at = -1;
	f->cpuctrl0_at = -1;
	f->wake_at = -1;
	f->barrier_at = -1;
	f->fail_alloc_idx = -1;
}

static u32 f_rd32(void *ctx, unsigned int off)
{
	struct fake *f = ctx ? ctx : fake;

	if (off == ANE_T6021_BOOT_REG_SCRATCH0)
		return f->scratch0_val;
	if (off == ANE_T6021_BOOT_REG_SCRATCH1)
		return f->scratch1_val;
	if (off == ANE_T6021_BOOT_REG_SCRATCH7) {
		if (f->s7_never_ack)
			return 0;
		if (f->n7 < (int)(sizeof(f->s7) / sizeof(f->s7[0])))
			return f->s7[f->n7++];
		return ANE_T6021_BOOT_ACK;
	}
	return 0;
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

static void f_phase(void *ctx, const char *what)
{
	struct fake *f = fake;

	(void)ctx;
	if (f->nphase < (int)(sizeof(f->phase) / sizeof(f->phase[0])))
		f->phase[f->nphase++] = what;
}

static int f_prepare(void *ctx, u32 *lo, u32 *hi)
{
	struct fake *f = fake;
	struct ane_t6021_boot_allocs a;
	int err;

	(void)ctx;
	f->prepare_at = f->nwr;
	/* THE SHARED ASSEMBLY — same function the kernel prepare calls:
	 * wrong-DVA, double-increment and unbounded-size classes must
	 * fail here, not on hardware. */
	err = ane_t6021_boot_prepare_publish(&asm_src, f->scratch3_req,
					     f->scratch0_val,
					     f->scratch1_captured, 0x4000,
					     ANE_T6021_BOOT_IPC_CEILING,
					     ANE_T6021_BOOT_HEAP_CEILING,
					     NULL, f_alloc2, &a, lo, hi);
	if (err)
		return err;
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

	/* ---- Init suballocation SOURCED fill (pass6 cd25b46, anchors
	 * G1-G26): static sources + post-READY dynamic heap/ordinal.
	 * TRUST BOUNDARY: scratch3_req is firmware-supplied and must pass
	 * ane_t6021_heap_size bounds before allocation. Pool word0
	 * semantics still open (selene lane) — the source is pinned, never
	 * a synthesized zero. ---- */
	{
		static const struct ane_t6021_init_sources src = {
			.fw_dva = 0x0000deadbeef000ULL,
			.ipc_dva = 0x00000badc0de000ULL,
			.cfg_size = 0x500000,
			.prev_fw_len = 0x1234, /* nonzero: full u32 must land at [0x30] */
			.heap_floor = 0x30000ULL,
			.pool_dma = 0x5555aaaab000ULL,
			.pool_word0 = 0x40000, /* MECHANISM-ONLY test value: the shipped [0x60] stays HARD-GATED (pf_pool_word0_proven) until pool+0x00 producer/consumer proof lands */
		};
		const u64 heap_size = ane_t6021_heap_size(0x8000, 0x30000,
							  0x300000000ULL);
		const u64 heap_dva = 0x0000feedface000ULL;
		const u32 ordinal = 7;
		u8 buf[ANE_T6021_INIT_STRUCT_SIZE];
		u8 leak[ANE_T6021_INIT_STRUCT_SIZE];
		static const u8 golden[ANE_T6021_INIT_STRUCT_SIZE] = {
		0x00, 0xf0, 0xee, 0xdb, 0xea, 0x0d, 0x00, 0x00, 0x00, 0xe0, 0x0d, 0xdc, 0xba, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb0, 0x0f, 0x00, 0x00, 0x00, 0x00,
		0x00, 0xe0, 0xac, 0xdf, 0xee, 0x0f, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x34, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb0, 0xaa, 0xaa, 0x55, 0x55, 0x00, 0x00,
		0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
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

		/* trust boundary: 0 request -> no heap; below floor -> floor;
		 * over ceiling -> refused */
		/* 'IPC ' size: max(DART page 0x4000, ordinal+1) */
		check(ane_t6021_ipc_size(0x4000, 0) == 0x4000,
		      "ipc size: first boot = one 16K DART page",
		      "max(0x4000, ordinal+1)");
		check(ane_t6021_ipc_size(0x4000, 7) == 0x4000,
		      "ipc size: ordinal below page size", "0x4000 wins");
		check(ane_t6021_ipc_size(0x4000, 0x5000) == 0x5001,
		      "ipc size: huge ordinal+1 dominates",
		      "ordinal+1 wins over page size");
		check(ane_t6021_heap_size(0, 0x30000,
					  ANE_T6021_BOOT_HEAP_CEILING) == 0,
		      "heap: request 0 -> no heap surface",
		      "pass6: 0 if request 0");
		check(ane_t6021_heap_size(0x8000, 0x30000,
					  ANE_T6021_BOOT_HEAP_CEILING) == 0x30000,
		      "heap: floor dominates small request",
		      "MAX(request, floor)");
		check(ane_t6021_heap_size(0x40000, 0x30000,
					  ANE_T6021_BOOT_HEAP_CEILING) == 0x40000,
		      "heap: request above floor passes",
		      "MAX(request, floor)");
		check(ane_t6021_heap_size(0x03000000, 0x30000,
					  ANE_T6021_BOOT_HEAP_CEILING) == -E2BIG,
		      "heap: request over the 32 MiB budget refused",
		      "operational budget, distinct from ABI width");
		check(ane_t6021_heap_size((u32)0x20000000ULL, 0x30000,
					  0x10000000ULL) == -E2BIG,
		      "heap: request over a tighter ceiling REFUSED",
		      "trust boundary before allocation");

		memset(buf, 0, sizeof(buf));
		ane_t6021_init_struct_fill(buf, &src, (long long)heap_size,
					   heap_dva, ordinal);
		check(memcmp(buf, golden, sizeof(buf)) == 0,
		      "sourced fill exact image",
		      "every fw-read field from pinned + runtime sources");

		check(rd_le64(buf) == src.fw_dva &&
		      rd_le64(buf + 0x08) == src.ipc_dva &&
		      rd_le64(buf + 0x10) == 0x500000ULL &&
		      rd_le64(buf + 0x18) == 0x0fb00000ULL &&
		      rd_le64(buf + 0x20) == heap_dva &&
		      rd_le64(buf + 0x28) == heap_size &&
		      rd_le32(buf + 0x30) == 0x1234 &&
		      rd_le64(buf + 0x50) == ordinal &&
		      rd_le64(buf + 0x58) == src.pool_dma &&
		      rd_le64(buf + 0x60) == 0x40000ULL &&
		      rd_le32(buf + ANE_T6021_INIT_COUNT_OFF) == 0x40 &&
		      rd_le32(buf + ANE_T6021_INIT_TEMPLATE_OFF +
			      ANE_T6021_INIT_TBIT_OFF) == 0x4,
		      "fill field reads",
		      "static + dynamic fields all decoded");

		/* untouched-zone check: gaps [0x34..37]/[0x64..67] and the
		 * kext-zero region [0x38..0x50) stay caller-owned */
		memset(leak, 0xa5, sizeof(leak));
		ane_t6021_init_struct_fill(leak, &src, 0x30000,
					   0x1234, 9);
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
		      "gaps preserved byte-exact (zero the block first)");
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
			.publish_barrier = f_dsb, .poll_wait = f_wait,
			.phase = f_phase,
			.prepare = f_prepare,
		};

		/* (1) gates closed -> -ENODATA and NOT ONE write */
		fake_reset(&fk);
		fake = &fk;
		{
			int cs = 0, fa = 0, bo = 0;
			u64 sres = 0;
			struct ane_t6021_boot_cfg cfg = {
				.preflight_ok = 0,
				.fw_dva = 0x500000,
			};

			check(ane_t6021_boot_run(&io, &cfg, &cs, &fa,
						 &bo, &sres) == -ENODATA,
			      "run: closed gate returns -ENODATA",
			      "no sequence started");
			check(fk.nwr == 0, "run: closed gate = no writes",
			      "Main: all gates before any boot write");
			check(cs == 0 && fa == 0 && bo == 0,
			      "run: closed gate sets no flags",
			      "state stays fenced");
		}

		/* (1b) live-fault gate OFF (2026-09-20 wedge): the whole
		 * sequence is skipped with -EAGAIN and ZERO writes —
		 * accidental-repeat prevention, before any write. */
		fake_reset(&fk);
		fk.rvbar = 0x1;
		{
			int cs = 0, fa = 0, bo = 0;
			u64 sres = 0;
			struct ane_t6021_boot_cfg cfg = {
				.preflight_ok = 1,
				.preboot_table_mode = 0,
				.fw_dva = 0x0000deadbeef000ULL,
			};

			check(ane_t6021_boot_run(&io, &cfg, &cs, &fa,
						 &bo, &sres) == -EAGAIN,
			      "run: table gate OFF = -EAGAIN",
			      "live-fault gate, zero writes");
			check(fk.nwr == 0 && cs == 0 && fa == 0 && bo == 0,
			      "gate-off: zero writes, no flags",
			      "accidental-repeat prevention");
		}

		/* (2) open gate, bit0-set RVBAR (live state), mode 2: TABLE
		 * SKIPPED (diagnostic) — grant tunables + full sequence. */
		fake_reset(&fk);
		fk.rvbar = 0x1;
		fk.scratch0_val = 0x05;      /* below 0x21 band */
		fk.scratch1_val = 0xab;
		fk.scratch3_req = 0x8000;    /* below floor */
		fk.scratch1_captured = 6;    /* ordinal 7 */
		{
			int cs = 0, fa = 0, bo = 0;
			u64 sres = 0;
			struct ane_t6021_boot_cfg cfg = {
				.preflight_ok = 1,
				.preboot_table_mode = 2,
				.fw_dva = 0x0000deadbeef000ULL,
			};
			int r = ane_t6021_boot_run(&io, &cfg, &cs, &fa,
						   &bo, &sres);

			check(r == 0, "run: happy path returns 0",
			      "fresh READY and DONE both observed");
			check(cs == 1 && fa == 1 && bo == 1,
			      "run: cpu_started/fw_alive/booted set",
			      "full sequence");
			check(fk.nwr == 28,
			      "run: 28 writes (12 grant + 8 clear + s6 + pulse2 + cpu2 + pub2 + wake; NO table)",
			      "mode-2 skip: full sequence minus table");
			check(fk.woff[0] == 0x000 && fk.wval[0] == 0x10U &&
			      fk.woff[11] == 0x430 &&
			      fk.wval[11] == 0x00001100U,
			      "grant tunables first (12 writes)",
			      "W8 APERTURE_UNLOCKED replay");
			check(fk.woff[12] == ANE_T6021_BOOT_REG_SCRATCH0 &&
			      fk.wval[12] == 0 &&
			      fk.woff[19] ==
			      ANE_T6021_BOOT_REG_SCRATCH7 &&
			      fk.wval[19] == 0,
			      "all eight scratch cells cleared",
			      "exact init clears ALL, not only 7");
			check(fk.woff[20] ==
			      ANE_T6021_BOOT_REG_SCRATCH6 &&
			      fk.wval[20] == 1 &&
			      fk.woff[21] ==
			      ANE_T6021_BOOT_REG_SCRATCH7 &&
			      fk.wval[21] == 1 &&
			      fk.woff[22] ==
			      ANE_T6021_BOOT_REG_SCRATCH7 &&
			      fk.wval[22] == 0,
			      "SCRATCH6=1 then SCRATCH7 pulse 1->0",
			      "stale ack cleared pre-CPU");
			check(fk.nw64 < 0,
			      "no RVBAR write on bit0-set branch",
			      "lawful skip; never write over bit0");
			check(fk.cpuctrl0_at == 23 &&
			      fk.wval[23] == 0 &&
			      fk.wval[24] == ANE_T6021_CPU_RUN_RELEASE,
			      "CPU_CONTROL 0 then 0x10 after pulse",
			      "strict order, both paths");
			check(fk.prepare_at == 25 &&
			      fk.barrier_at == 25,
			      "prepare+dsb after poll A",
			      "publication strictly post-alive");
			check(fk.woff[25] ==
			      ANE_T6021_BOOT_REG_SCRATCH0 &&
			      fk.wval[25] == (u32)(0x5555aaaab000ULL & 0xffffffffU) &&
			      fk.woff[26] ==
			      ANE_T6021_BOOT_REG_SCRATCH1 &&
			      fk.wval[26] == (u32)(0x5555aaaab000ULL >> 32),
			      "publish low32/high32 from prepare",
			      "suballoc DVA halves");
			check(fk.woff[27] ==
			      ANE_T6021_BOOT_REG_SCRATCH7 &&
			      fk.wval[27] == ANE_T6021_BOOT_WAKE_REQ,
			      "wake after publish", "releases fw wait");
			check(fk.nasz == 3 &&
			      fk.asz[0] == 0x40000 &&
			      fk.asz[1] == 0x4000 &&
			      fk.asz[2] == 0x30000,
			      "SHARED assembly alloc sizes",
			      "pool 0x40000; ipc max(0x4000,6+1); heap max(req,floor)");
			check(fk.aiov[2] == 0x0000feedface000ULL,
			      "SHARED assembly heap DVA",
			      "fill sees the ALLOCATED iova (no NULL publish)");
			check(rd_le64(fake_mem + 0x58) == fk.aiov[0],
			      "publish header [0x58] = ALLOCATED pool DVA",
			      "not the zeroed input source (Main zero-DVA bug)");
			check(sres == 0x000000ab00000005ULL,
			      "DONE result captured raw (SC1<<32|SC0)",
			      "exposed, not discarded (Main review)");
		}

		/* (2b) IPC double-increment catch: captured 0x3FFF makes
		 * the correct ordinal+1 exactly one DART page (0x4000);
		 * a +2 bug would allocate 0x4001. */
		fake_reset(&fk);
		fk.rvbar = 0x1;
		fk.scratch1_captured = 0x3fff;
		{
			int cs = 0, fa = 0, bo = 0;
			u64 sres = 0;
			struct ane_t6021_boot_cfg cfg = {
				.preflight_ok = 1,
				.preboot_table_mode = 2,
				.fw_dva = 0x0000deadbeef000ULL,
			};

			check(ane_t6021_boot_run(&io, &cfg, &cs, &fa,
						 &bo, &sres) == 0,
			      "run: 0x3FFF captured boots",
			      "ordinal boundary");
			check(fk.asz[1] == 0x4000,
			      "IPC size = ordinal+1 EXACT (no double add)",
			      "max(0x4000, 0x3FFF+1) == 0x4000");
		}

		/* (2c) SCRATCH0 band boundary: 0x20 accepted (below
		 * threshold), 0x21 REFUSED before ANY allocation. */
		fake_reset(&fk);
		fk.rvbar = 0x1;
		fk.scratch0_val = 0x20;
		fk.scratch1_val = 6;
		fk.scratch3_req = 0x8000;
		{
			int cs = 0, fa = 0, bo = 0;
			u64 sres = 0;
			struct ane_t6021_boot_cfg cfg = {
				.preflight_ok = 1,
				.preboot_table_mode = 2,
				.fw_dva = 0x0000deadbeef000ULL,
			};

			check(ane_t6021_boot_run(&io, &cfg, &cs, &fa,
						 &bo, &sres) == 0,
			      "run: scratch0=32 accepted",
			      "boundary below threshold");
			check(fk.nasz == 3,
			      "scratch0=32 allocates pool/ipc/heap",
			      "MAX path taken");
		}
		fake_reset(&fk);
		fk.rvbar = 0x1;
		fk.scratch0_val = 0x21;
		fk.scratch1_val = 6;
		{
			int cs = 0, fa = 0, bo = 0;
			u64 sres = 0;
			struct ane_t6021_boot_cfg cfg = {
				.preflight_ok = 1,
				.preboot_table_mode = 2,
				.fw_dva = 0x0000deadbeef000ULL,
			};

			check(ane_t6021_boot_run(&io, &cfg, &cs, &fa,
						 &bo, &sres) == -EPROTO,
			      "run: scratch0=33 REFUSED (-EPROTO)",
			      "Main raw trace 0x95ea2a0->0x3330");
			check(cs == 1 && fk.nasz == 0,
			      "refusal BEFORE allocations/publication",
			      "post-READY refusal; wedged-pin holds");
			check(ane_t6021_boot_dma_reclaimable(cs) == false,
			      "refusal: DMA held (wedged-pin)",
			      "reboot reclaims");
		}

		/* (3) bit0-clear RVBAR: the fold write64 appears */
		fake_reset(&fk);
		{
			int cs = 0, fa = 0, bo = 0;
			u64 sres = 0;
			struct ane_t6021_boot_cfg cfg = {
				.preflight_ok = 1,
				.preboot_table_mode = 2,
				.fw_dva = 0x0000deadbeef000ULL,
			};

			check(ane_t6021_boot_run(&io, &cfg, &cs, &fa,
						 &bo, &sres) == 0,
			      "run: unprogrammed branch boots",
			      "direct-image fold path");
			/* DONE result capture: the fake returns scratch1
			 * read = 0 -> hi word 0; scratch0 read = 0 -> lo 0.
			 * Nonzero capture is asserted in case (2). */
			check(sres == 0, "DONE result captured",
			      "raw u64 exposed, not discarded");
			check(fk.nw64 == 23 &&
			      fk.w64val ==
			      ane_t6021_rvbar_compose(cfg.fw_dva),
			      "RVBAR write64 = entry fold",
			      "after grant+scratch pulse, before CPU release");
		}

		/* (4) poll A timeout: HOLD — started, nothing published */
		fake_reset(&fk);
		fk.s7_never_ack = 1;
		{
			int cs = 0, fa = 0, bo = 0;
			u64 sres = 0;
			struct ane_t6021_boot_cfg cfg = {
				.preflight_ok = 1,
				.preboot_table_mode = 2,
				.fw_dva = 0x0000deadbeef000ULL,
			};

			check(ane_t6021_boot_run(&io, &cfg, &cs, &fa,
						 &bo, &sres) == -ETIMEDOUT,
			      "run: poll A timeout = -ETIMEDOUT",
			      "bounded polls");
			check(cs == 1 && fa == 0 && bo == 0,
			      "timeout: cpu_started, not alive/booted",
			      "HOLD semantics");
			check(fk.prepare_at < 0 && fk.nasz == 0,
			      "timeout: no publish, NO allocations",
			      "no partial boot past poll A");
			check(fk.nwr == 26,
			      "timeout: writes stop after CPU release (26 = 25 + fold wr64)",
			      "no publish/wake after poll A timeout");
			check(ane_t6021_boot_dma_reclaimable(cs) == false,
			      "ownership: DMA NOT reclaimable while started",
			      "cannot free while the CPU may fetch");
		}

		/* (5) fw-start-debug stop_after bisect (2026-09-22):
		 * stop AFTER step N returns -ECANCELED, never splits a
		 * step, and a poll-A timeout inside step 4 stays
		 * -ETIMEDOUT. Bit0-set branch throughout: no fold write. */
		fake_reset(&fk);
		fk.rvbar = 0x1;
		{
			int cs = 0, fa = 0, bo = 0;
			u64 sres = 0;
			struct ane_t6021_boot_cfg cfg = {
				.preflight_ok = 1,
				.preboot_table_mode = 2,
				.fw_dva = 0x0000deadbeef000ULL,
				.stop_after = 1,
			};

			check(ane_t6021_boot_run(&io, &cfg, &cs, &fa,
						 &bo, &sres) == -ECANCELED,
			      "stop_after=1 returns -ECANCELED",
			      "tunables done, nothing else fired");
			check(fk.nwr == 12 && cs == 0 && fa == 0 && bo == 0,
			      "stop_after=1 = exactly the 12 tunables",
			      "no CPU, no scratch");
			check(fk.woff[0] == 0x000 && fk.woff[11] == 0x430,
			      "stop_after=1 order intact",
			      "first 0x000, last 0x430");
		}
		fake_reset(&fk);
		fk.rvbar = 0x1;
		{
			int cs = 0, fa = 0, bo = 0;
			u64 sres = 0;
			struct ane_t6021_boot_cfg cfg = {
				.preflight_ok = 1,
				.preboot_table_mode = 2,
				.fw_dva = 0x0000deadbeef000ULL,
				.stop_after = 2,
			};

			check(ane_t6021_boot_run(&io, &cfg, &cs, &fa,
						 &bo, &sres) == -ECANCELED,
			      "stop_after=2 returns -ECANCELED",
			      "scratch clear+pulse done");
			check(fk.nwr == 23 &&
			      fk.wval[22] == 0 &&
			      fk.woff[22] == ANE_T6021_BOOT_REG_SCRATCH7,
			      "stop_after=2 ends on the pulse (23 writes)",
			      "12 + 8 clear + s6 + pulse2; CPU untouched");
		}
		fake_reset(&fk);
		fk.rvbar = 0x1;
		{
			int cs = 0, fa = 0, bo = 0;
			u64 sres = 0;
			struct ane_t6021_boot_cfg cfg = {
				.preflight_ok = 1,
				.preboot_table_mode = 2,
				.fw_dva = 0x0000deadbeef000ULL,
				.stop_after = 3,
			};

			check(ane_t6021_boot_run(&io, &cfg, &cs, &fa,
						 &bo, &sres) == -ECANCELED,
			      "stop_after=3 returns -ECANCELED",
			      "rvbar decision recorded (skip branch)");
			check(fk.nwr == 23 && fk.nw64 < 0,
			      "stop_after=3 adds no write on bit0-set",
			      "decision is the read; RUN never fired");
		}
		fake_reset(&fk);
		fk.rvbar = 0x1;
		{
			int cs = 0, fa = 0, bo = 0;
			u64 sres = 0;
			struct ane_t6021_boot_cfg cfg = {
				.preflight_ok = 1,
				.preboot_table_mode = 2,
				.fw_dva = 0x0000deadbeef000ULL,
				.stop_after = 4,
			};

			check(ane_t6021_boot_run(&io, &cfg, &cs, &fa,
						 &bo, &sres) == -ECANCELED,
			      "stop_after=4 READY returns -ECANCELED",
			      "fw alive, publish/wake withheld");
			check(cs == 1 && fa == 1 && bo == 0,
			      "stop_after=4: started+alive, not booted",
			      "wedged-pin semantics apply");
			check(fk.nwr == 25 && fk.prepare_at < 0 &&
			      fk.nasz == 0,
			      "stop_after=4 = 25 writes, NO publish/alloc",
			      "23 + CPU_CONTROL 0 + 0x10");
			check(ane_t6021_boot_dma_reclaimable(cs) == false,
			      "stop_after=4: DMA held",
			      "CPU may be fetching");
		}
		fake_reset(&fk);
		fk.rvbar = 0x1;
		fk.s7_never_ack = 1;
		{
			int cs = 0, fa = 0, bo = 0;
			u64 sres = 0;
			struct ane_t6021_boot_cfg cfg = {
				.preflight_ok = 1,
				.preboot_table_mode = 2,
				.fw_dva = 0x0000deadbeef000ULL,
				.stop_after = 4,
			};

			check(ane_t6021_boot_run(&io, &cfg, &cs, &fa,
						 &bo, &sres) == -ETIMEDOUT,
			      "stop_after=4 timeout stays -ETIMEDOUT",
			      "the timeout is the answer, not the stop");
			check(cs == 1 && fk.nwr == 25,
			      "timeout writes = same 25",
			      "stop_after cannot silence a timeout");
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
