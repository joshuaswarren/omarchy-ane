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

	/* ---- Init suballocation SOURCED fill (pass5b/5c/5d corrected
	 * contract, b8b7c35; validator check_init_contract.py 64/64):
	 * [0x00]=FWIM DVA, [0x08]=IPC surface DVA, [0x10]=config size
	 * (u32 zext; = image byte-count), [0x18]=0x10000000-size,
	 * [0x58]=pool DMA base, [0x60]=pool word0, [0x68]=64 count,
	 * template[0x00]=0, template+0xC0=4. OPEN [0x20]/[0x28]/[0x30]/
	 * [0x50] written by nobody; gaps [0x34..37]/[0x64..67] need a
	 * zeroed block; publication stays fenced. ---- */
	{
		static const struct ane_t6021_init_sources src = {
			.fw_dva = 0x0000deadbeef000ULL,
			.ipc_dva = 0x00000badc0de000ULL,
			.cfg_size = 0x500000, /* config+0x138 byte-count (Main, audit 751caa4) */
			.pool_dma = 0x5555aaaab000ULL,
			.pool_word0 = 0,
		};
		u8 buf[ANE_T6021_INIT_STRUCT_SIZE];
		u8 leak[ANE_T6021_INIT_STRUCT_SIZE];
		size_t i;
		int touched;

		/* geometry invariants */
		check(ANE_T6021_INIT_COUNT_OFF + 4 +
			      ANE_T6021_INIT_TEMPLATE_SIZE ==
			      0x16c,
		      "template end 0x16c", "0x68+4+0x100");
		check(0x16c + 8 == ANE_T6021_INIT_STRUCT_SIZE,
		      "struct size 0x174", "0x16c + header qword");

		memset(buf, 0, sizeof(buf));
		ane_t6021_init_struct_fill(buf, &src);

		/* golden image generated independently (python struct pack of
		 * the six sources + count + tbit over a zeroed 0x174 block);
		 * cfg_size = 0x500000 -> [0x18] = 0x0fb00000 (Main, 751caa4) */
		static const u8 golden[ANE_T6021_INIT_STRUCT_SIZE] = {
		0x00, 0xf0, 0xee, 0xdb, 0xea, 0x0d, 0x00, 0x00, 0x00, 0xe0, 0x0d, 0xdc, 0xba, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb0, 0x0f, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb0, 0xaa, 0xaa, 0x55, 0x55, 0x00, 0x00,
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
		check(memcmp(buf, golden, sizeof(buf)) == 0,
		      "sourced fill exact image",
		      "all six sources + count + tbit, rest zero");

		check(rd_le64(buf) == src.fw_dva &&
		      rd_le64(buf + 0x08) == src.ipc_dva &&
		      rd_le64(buf + 0x10) == 0x500000ULL &&
		      rd_le64(buf + 0x18) == 0x0fb00000ULL &&
		      rd_le32(buf + ANE_T6021_INIT_COUNT_OFF) == 0x40 &&
		      rd_le32(buf + ANE_T6021_INIT_TEMPLATE_OFF +
			      ANE_T6021_INIT_TBIT_OFF) == 0x4,
		      "fill field reads", "LE decode incl. size complement");

		/* open fields ([0x20..0x58) incl. [0x30] fw-load progress and
		 * [0x50]) and the gap bytes stay EXACTLY as the caller left
		 * them - no hidden writes, no validity claim over fw-read units */
		memset(leak, 0xa5, sizeof(leak));
		ane_t6021_init_struct_fill(leak, &src);
		touched = 0;
		for (i = 0; i < sizeof(leak); i++) {
			int closed = i < 0x20 ||
				     (i >= 0x58 && i < 0x70) ||
				     i == (ANE_T6021_INIT_TEMPLATE_OFF +
					   ANE_T6021_INIT_TBIT_OFF);

			if (!closed && leak[i] != 0xa5)
				touched++;
		}
		check(touched == 0, "fill touches sourced fields only",
		      "open fields + gaps preserved byte-exact (zero first)");

		/* determinism: zero sources differ only in the written fields */
		memset(leak, 0xff, sizeof(leak));
		memset(leak, 0, sizeof(leak));
		ane_t6021_init_struct_fill(leak, &(struct ane_t6021_init_sources){
			.cfg_size = 0x500000 });
		check(rd_le64(leak) == 0 && rd_le64(leak + 0x08) == 0 &&
		      rd_le64(leak + 0x10) == 0x500000ULL &&
		      rd_le64(leak + 0x18) == 0x0fb00000ULL,
		      "fill with zero DVAs/pool", "zeroed-source variant");
	}

	printf("%s: %d checks, %d failures\n",
	       failures ? "FAILED" : "PASSED", checks, failures);
	return failures != 0;
}
