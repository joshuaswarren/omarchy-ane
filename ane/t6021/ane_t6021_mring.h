/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * ane_t6021_mring.h: what an entry of the firmware's SHAREDMALLOC ring (ChMan channel 5, 8 slots of 0x40 bytes) is, factored out so the host unit test
 * (tools/test_t6021_mring.c) compiles the rule the driver runs.
 *
 * Observed on selene 13v5 (shared-memory requests in the firmware image, free requests in the lab notes): the firmware writes one slot per request, three
 * 64-bit words (header, size, mode), and the host answers by writing the header back with bit 0 set and ringing doorbell bit 5.
 *   MALLOC: header 0, size = bytes, mode 0 or 1.
 *   FREE or UNREMAP: header = the IOVA being given back (16 KiB aligned, so bit 0 is clear and the header is not 0),
 *     both body words 0. The firmware has already unmapped it on its side and waits for the answer with no timeout, holding its shared-memory lock.
 *   A slot with bit 0 set is host-owned: it was already answered.
 * A slot with header 0 and size 0 carries no request (a slot the firmware has not written yet). A REMAP request has that shape too (header 0, size 0,
 * mode) and is not recognised here.
 */
#ifndef __ANE_T6021_MRING_H__
#define __ANE_T6021_MRING_H__

#include <linux/types.h>

enum ane_t6021_mring_kind {
	ANE_T6021_MRING_IDLE,		/* nothing posted in this slot */
	ANE_T6021_MRING_MALLOC,		/* allocate size bytes, answer with the buffer address */
	ANE_T6021_MRING_FREE,		/* the firmware returns the IOVA in the header: answer, release nothing the firmware reads */
	ANE_T6021_MRING_ANSWERED,	/* host-owned slot, already answered */
	ANE_T6021_MRING_BAD,		/* a shape no request has: stop, the ring is not what we think */
};

static inline enum ane_t6021_mring_kind ane_t6021_mring_classify(u64 hdr, u64 size, u64 mode)
{
	if (hdr & 1)
		return ANE_T6021_MRING_ANSWERED;
	if (!hdr) {
		if (!size)
			return ANE_T6021_MRING_IDLE;
		return mode > 0xffffffffULL ? ANE_T6021_MRING_BAD : ANE_T6021_MRING_MALLOC;
	}
	return (size || mode) ? ANE_T6021_MRING_BAD : ANE_T6021_MRING_FREE;
}

#endif /* __ANE_T6021_MRING_H__ */
