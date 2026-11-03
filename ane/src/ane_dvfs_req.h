/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * The ANE's part of the PMP PTD power-state request word (PS-REQ, PTD register 0xf8).
 *
 * The word is shared: its other bits belong to other consumers of the same PMP (display, ISP, video decode, USB4 and, with the AGX vote, bit 9). The ANE
 * owns bit 10 (ANE_SYS, index 10 of the PMP soc-device table) and nothing else, so every decision here looks at bit 10 only. An earlier version of the
 * level code required the exact word 0x60003000 and gave up ("ps-req-unexpected") when any other bit was set, which cost the whole ANE clock level
 * whenever the GPU vote or a video decode was active at ANE resume.
 *
 * Pure functions on the word, no kernel calls. ane/test/dvfs_model_test.py compiles ane_dvfs.c with this header against a fake PTD (the bit 9 and other-bit cases).
 */
#ifndef ANE_DVFS_REQ_H
#define ANE_DVFS_REQ_H

#include <linux/types.h>

#define PS_ANE_SYS	((u64)1 << 10)

/* The up sequence may start: this driver's request is not set yet. */
static inline bool ane_dvfs_req_may_up(u64 req)
{
	return !(req & PS_ANE_SYS);
}

static inline u64 ane_dvfs_req_up(u64 req)
{
	return req | PS_ANE_SYS;
}

static inline u64 ane_dvfs_req_down(u64 req)
{
	return req & ~PS_ANE_SYS;
}

/* After the acknowledge: the word reads back with the request set. Other bits may have changed meanwhile and are not this driver's business. */
static inline bool ane_dvfs_req_is_up(u64 req)
{
	return !!(req & PS_ANE_SYS);
}

#endif /* ANE_DVFS_REQ_H */
