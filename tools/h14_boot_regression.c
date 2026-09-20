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
	check(ane_t6021_rvbar_compose(0x400000) == 0x0081000000400001ULL,
	      "compose(0x400000)", "FW_BUF-size surface vector");
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

	/* ---- Init suballocation CLOSED-FIELD fill (0x174: header[0x00]
	 * u64 = fw DVA (legacy branch), [0x68] u32 = 64 count. PASS5
	 * (c364f24, Main-corrected): the fw CONSUMES [0x08]..[0x68] —
	 * [0x08] = *(dev+0x988+0x18), [0x10] = config size, [0x18] =
	 * 0x10000000-config.size. template+0xC0 = 4 RESOLVED (Main raw
	 * anchors 0x9612b78/7c/80). The fill writes [0x00], [0x68] and
	 * template+0xC0 only; the remaining fw-consumed fields have no
	 * pinned Linux source and this test asserts NO full-init
	 * validity: publication stays fenced. ---- */
	{
		u8 buf[ANE_T6021_INIT_STRUCT_SIZE];
		u8 expected[ANE_T6021_INIT_STRUCT_SIZE];
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
		ane_t6021_init_struct_fill(buf, 0xdeadbeef000ULL);

		memset(expected, 0, sizeof(expected));
		expected[0] = 0x00; expected[1] = 0xf0;
		expected[2] = 0xee; expected[3] = 0xdb;
		expected[4] = 0xea; expected[5] = 0x0d; /* 0xdeadbeef000 LE */
		expected[0x68] = 0x40; /* count 64 */
		expected[0x12c] = 0x04; /* template+0xC0 = 4 (RESOLVED) */
		check(memcmp(buf, expected, sizeof(buf)) == 0,
		      "fill writes resolved fields only",
		      "fw-consumed open zone untouched");

		check(rd_le64(buf) == 0xdeadbeef000ULL &&
		      rd_le32(buf + ANE_T6021_INIT_COUNT_OFF) == 0x40 &&
		      rd_le32(buf + ANE_T6021_INIT_TEMPLATE_OFF +
			      ANE_T6021_INIT_TBIT_OFF) == 0x4,
		      "fill field reads", "LE decode incl. tbit");

		/* the fill itself must not touch anything but [0x00..0x08)
		 * and [0x68..0x6c): the fw-consumed open zone and the
		 * disputed template zone stay exactly as the caller left
		 * them (zero here) — no hidden writes, no validity claim */
		memset(leak, 0xa5, sizeof(leak));
		ane_t6021_init_struct_fill(leak, 0x1234);
		touched = 0;
		for (i = 0; i < sizeof(leak); i++) {
			int in_closed = i < 8 ||
					(i >= ANE_T6021_INIT_COUNT_OFF &&
					 i < ANE_T6021_INIT_COUNT_OFF + 4) ||
					i == (ANE_T6021_INIT_TEMPLATE_OFF +
					      ANE_T6021_INIT_TBIT_OFF);

			if (!in_closed && leak[i] != 0xa5)
				touched++;
		}
		check(touched == 0, "fill touches closed fields only",
		      "caller-owned zones preserved byte-exact");

		/* determinism: DVA=0 variant differs only in the header */
		memset(expected, 0, 8); /* header qword = 0 */
		memset(leak, 0xff, sizeof(leak));
		memset(leak, 0, sizeof(leak));
		ane_t6021_init_struct_fill(leak, 0);
		check(rd_le64(leak) == 0 &&
		      memcmp(leak, expected, sizeof(leak)) == 0,
		      "fill with fw DVA 0", "zeroed-header variant");
	}

	printf("%s: %d checks, %d failures\n",
	       failures ? "FAILED" : "PASSED", checks, failures);
	return failures != 0;
}
