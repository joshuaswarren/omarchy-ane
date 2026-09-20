// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Joshua Warren / mlx-omarchy contributors. */

/* Host-side failing check for the pre-submit power guard (no hardware).
 * Feeds the exact SET-block ACTUAL words from the 2026-09-20 m1-test-host
 * first-load wedge (set0 on, base/set1..4 gated: aggregate 0xf) and
 * from the 2026-09-16 five-provider proven-power captures (all words
 * 0xf0: aggregate 0xffffff) through the same aggregate/predicate the
 * driver's submit path consults. The `make check` target also fails
 * when ane_submit no longer consults the guard before ane_tm_enqueue,
 * which is the property that turns a gated-island submit into a clean
 * -ENODEV instead of a -110 timeout and wedge. */

#include <stdio.h>

#include "ane_ps.h"

static int failures;

static void expect(const char *name, u32 act, bool on)
{
	if (ane_ps_islands_on(act) != on) {
		printf("FAIL %s: act=%#x expected %s\n", name, act,
		       on ? "powered" : "refused");
		failures++;
	} else {
		printf("ok   %s: act=%#x %s\n", name, act,
		       on ? "powered" : "refused");
	}
}

static u32 agg(const u32 words[ANE_PS_WORDS])
{
	return ane_ps_aggregate(words);
}

int main(void)
{
	/* 2026-09-16 five-provider boot, powered: every ACTUAL nibble f. */
	const u32 powered[ANE_PS_WORDS] = { 0xf0, 0xf0, 0xf0, 0xf0, 0xf0,
					    0xf0 };

	/* 2026-09-20 m1-test-host first-load wedge: set0 powered, base and
	 * set1..4 gated (no island providers attached). */
	const u32 wedged[ANE_PS_WORDS] = { 0xf0, 0x00, 0x00, 0x00, 0x00,
					   0x00 };

	/* 2026-09-16 force_suspend mid-states observed in dmesg
	 * (one provider cycle per word). */
	const u32 susp_w2[ANE_PS_WORDS] = { 0xf0, 0xf0, 0x00, 0xf0, 0xf0,
					    0xf0 }; /* 0xfff0ff */
	const u32 susp_w2w3[ANE_PS_WORDS] = { 0xf0, 0xf0, 0x00, 0x00, 0xf0,
					      0xf0 }; /* 0xff00ff */

	/* Only bits 4-7 of each word are ACTUAL: surrounding bits must
	 * not flip the decision. 0xff0 -> nibble f; 0x3c -> nibble 3. */
	const u32 noisy[ANE_PS_WORDS] = { 0x0ff0, 0x003c, 0xf0, 0xf0, 0xf0,
					  0xf0 };

	if (ANE_PS_ALL_ON != 0xffffffu) {
		printf("FAIL all_on: ANE_PS_ALL_ON=%#x expected 0xffffff\n",
		       ANE_PS_ALL_ON);
		failures++;
	} else {
		printf("ok   all_on: ANE_PS_ALL_ON=0xffffff\n");
	}

	expect("powered-20260916", agg(powered), true);
	expect("wedged-m1-test-host-20260920", agg(wedged), false);
	expect("suspend-w2-20260916", agg(susp_w2), false);
	expect("suspend-w2w3-20260916", agg(susp_w2w3), false);
	expect("noisy-actual-bits", agg(noisy), false);

	if (failures) {
		printf("%d check(s) FAILED\n", failures);
		return 1;
	}
	printf("all power-guard checks passed\n");
	return 0;
}
