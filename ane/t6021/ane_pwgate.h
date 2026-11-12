/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * ane_pwgate.h - what the T8112 probe may do about a closed ANE power gate.
 *
 * The vendor kext (its clock-and-power enable, selector 0x70) writes 0 to PWGATE
 * (the "set" window + 0x8b8) and waits for bits 29:28 to read 0
 * (receipts/2026-10-01-t8112-ane). The driver never wrote it. With the
 * pwgate_open module parameter it writes once, and only when the word reads
 * exactly the closed value the boot firmware leaves (0x30000000). Any other
 * word, including an unmapped window (U32_MAX), refuses without a write.
 *
 * Pure function shared by the kernel probe and tools/test_ane_pwgate.c.
 * Consumers provide u32 and bool. This header includes nothing.
 */
#ifndef __ANE_PWGATE_H__
#define __ANE_PWGATE_H__

#define ANE_PWGATE_CLOSED_MASK	0x30000000U	/* bits 29:28 */
#define ANE_PWGATE_CLOSED_WORD	0x30000000U
#define ANE_PWGATE_WAIT_US	100000

enum ane_pwgate_plan {
	ANE_PWGATE_OPEN,	/* bits 29:28 already clear: go on */
	ANE_PWGATE_WRITE,	/* write 0 once, then wait */
	ANE_PWGATE_REFUSE,	/* refuse before any engine read, no write */
};

static inline enum ane_pwgate_plan ane_pwgate_plan(u32 gate, bool opt_in)
{
	if (!(gate & ANE_PWGATE_CLOSED_MASK))
		return ANE_PWGATE_OPEN;
	if (opt_in && gate == ANE_PWGATE_CLOSED_WORD)
		return ANE_PWGATE_WRITE;
	return ANE_PWGATE_REFUSE;
}

#endif /* __ANE_PWGATE_H__ */
