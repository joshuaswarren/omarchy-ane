// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2026 Joshua Warren / mlx-omarchy contributors. */

#ifndef __ANE_PS_H__
#define __ANE_PS_H__

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t u32;
#endif

/* The ane SET block (m1n1 ANE.ps_map) maps the pmgr power-state words
 * for this engine: set0, base, set1..4 (ANE_PS_WORDS). pmgr reads are
 * always safe; the words themselves are firmware-locked: a direct
 * write external-aborts the SoC (netconsole-named on T6001 2026-09-16;
 * the same write site hard-reset T8103 in the 95dbcf3 era). No code
 * ever writes this block. */
#define ANE_PS_ACTUAL_MASK	  0xf0
#define ANE_PS_WORDS		  6 /* set0, base, set1..4 */
#define ANE_PS_ALL_ON		  ((1U << (4 * ANE_PS_WORDS)) - 1)

/* ACTUAL nibble of each SET word, word 0 in the low nibble. Only bits
 * 4-7 of each raw pmgr word are the ACTUAL field; anything else in the
 * word is ignored. */
static inline u32 ane_ps_aggregate(const u32 words[ANE_PS_WORDS])
{
	u32 v = 0;
	int i;

	for (i = 0; i < ANE_PS_WORDS; i++)
		v |= ((words[i] & ANE_PS_ACTUAL_MASK) >> 4) << (i * 4);
	return v;
}

/* Qualified layouts are all-on or not: the islands read powered on
 * only at ANE_PS_ALL_ON (0xffffff). Journal evidence 2026-09-20 (m1-test-host
 * first load): gated islands aggregate 0xf (set0 on, base/set1..4
 * off) alongside a -110 timeout and wedge; causation is pending the
 * restored-provider smoke. Evidence 2026-09-16 (five-provider boot):
 * the powered state reads 0xffffff through Linux genpd cycles. There
 * is no per-SoC partial-on layout: t8103 and t6000 are qualified at
 * the same six words. */
static inline bool ane_ps_islands_on(u32 act)
{
	return act == ANE_PS_ALL_ON;
}

#endif /* __ANE_PS_H__ */
