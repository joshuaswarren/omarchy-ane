// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* T6021 ANE transport core — MBI handshake (SCRATCH wake -> fw channel
 * table), capture of the fw->host message registers, MGMT decode for
 * received words, RTBuddy app-endpoint ring bookkeeping.
 *
 * The h14g transport is NOT the m1n1 ASC mailbox: the K14 kext never
 * touches any +0x8xxx-family register, and the +0x1608114 analogy read
 * SError-aborted jw14m2 (2026-09-19).  The kext-evidenced sequence
 * (InitializeRTBuddy 0x…95e942c) is: hand a command buffer via
 * SCRATCH0/1 (+0x1840048/+0x184004c), write the wake word 0xf7fbdff9
 * to SCRATCH7 (+0x1840064), poll until the fw overwrites it with
 * 0x80402006 ("channel description table ready"), read the table base
 * back from SCRATCH0/1, then register each {type,bit,size,phys} entry
 * with the doorbell setter (write32(1 << bit) to +0x1844000).  This
 * build runs the handshake and CAPTURES the results; it never rings
 * the doorbell — the channel bit for MGMT/INIT comes out of the table
 * dump first (W4-fix follow-up).
 */

#include <linux/bitmap.h>
#include <linux/dev_printk.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <asm/memory.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/minmax.h>

#include <asm/barrier.h>

#include "ane_t6021.h"

/* ---- MBI transport (capture-only; kext sites cited inline) ---- */

static u32 ane_mbi_scratch_get(struct ane_t6021 *ane, unsigned int i)
{
	return readl(ane->base[ANE_T6021_REG_ENGINE] +
		     ANE_MBI_SCRATCH0 + 4 * i);
}

static void ane_mbi_msgregs_dump(struct ane_t6021 *ane, const char *when)
{
	void __iomem *eng = ane->base[ANE_T6021_REG_ENGINE];

	dev_info(ane->dev,
		 "MBI msgregs %s: i2a=%08x_%08x a2i_rd=%08x a2i_wr=%08x\n",
		 when,
		 readl(eng + ANE_MBI_MSG_I2A_HI),
		 readl(eng + ANE_MBI_MSG_I2A_LO),
		 readl(eng + ANE_MBI_MSG_A2I_RD),
		 readl(eng + ANE_MBI_MSG_A2I_WR));
}

/* MBI capture: read-only.  The SCRATCH-handshake WRITE path (cmd
 * buffer -> SCRATCH0/1, wake 0xf7fbdff9 -> SCRATCH7) is FATAL on this
 * silicon in RTBuddy mode: the first write32 to SCRATCH0 (+0x1840048)
 * SError-aborted CPU4 (code 0xbe000000, unclean reset) on jw14m2
 * 2026-09-19, receipted by netconsole with per-stage flush points
 * (all eight SCRATCH pre-reads logged clean immediately before).  The
 * kext's SCRATCH flow (InitializeRTBuddy 0x…95eaa94-0x…95eaf00) is the
 * fw-sideload boot mode's init, not the RTBuddy host attach; with
 * selene running, the fw-protected control surface rejects host
 * writes with an async external abort while reads stub clean.  No
 * engine-window write exists in this driver again. */
int ane_t6021_mbi_boot(struct ane_t6021 *ane)
{
	struct device *dev = ane->dev;
	int i;

	if (ane->mbi_table_ready)
		return 0;

	for (i = 0; i < 8; i++)
		dev_info(dev, "MBI scratch%d pre=%08x\n", i,
			 ane_mbi_scratch_get(ane, i));

	ane_mbi_msgregs_dump(ane, "attach");
	ane->mbi_table_ready = true;

	/* Next lane (static): EnableRTBuddyEndpoints 0x…95fec30 waits up
	 * to 10 s (ns constant 0x2_540be400 = 10^10 into the wait
	 * helper 0x964c038; 0x5f5e100 = 10^8 and 0x1dcd6500 = 5e8 at the
	 * other call sites) for a service object and takes the RTBuddy
	 * gate from [service+0x88] — the TX path lives behind that
	 * service, not in an ANE-window register.  Carve the full
	 * kernelcache.release.mac14j for the publishing kext before any
	 * further device write. */
	dev_info(dev,
		 "MBI wall: SCRATCH/msgreg surfaces are read-only (host write = SError, 2026-09-19); transport pinned next lane from the provider kext\n");
	return 0;
}

/* fw->host message registers: capture-only read (kext reads the pair
 * at 0x…95ee6a8/0x…9605d20; no FIFO semantics pinned yet) */
static void ane_mbi_drain(struct ane_t6021 *ane)
{
	ane_mbi_msgregs_dump(ane, "drain");
}

void ane_t6021_rtkit_drain(struct ane_t6021 *ane)
{
	mutex_lock(&ane->mbox_lock);
	ane_mbi_drain(ane);
	mutex_unlock(&ane->mbox_lock);
}

/* ---- RTBuddy app endpoints (W2 §3) ---- */

static const struct ane_t6021_ep ane_ep_config[ANE_T6021_EP_COUNT] = {
	[1] = { .id = 1, .name = "INIT", .fourcc = 0x494e4954,
		.ring_size = 0x10000 },
	[2] = { .id = 2, .name = "T2FC", .fourcc = 0x54324643,
		.ring_size = 0x40000 },
	[3] = { .id = 3, .name = "T2FH", .fourcc = 0x54324648,
		.ring_size = 0x40000 },
	[4] = { .id = 4, .name = "T2HS", .fourcc = 0x54324853,
		.ring_size = 0x10000 },
	[5] = { .id = 5, .name = "T2HC", .fourcc = 0x54324843,
		.ring_size = 0x20000 },
	[6] = { .id = 6, .name = "T2HT", .fourcc = 0x54324854,
		.ring_size = 0x10000 },
};

/* ---- CSNE_CMD submission on the INIT channel (W4) ---- */

int ane_t6021_csne_submit(struct ane_t6021 *ane, const void *cmd, size_t size)
{
	struct ane_t6021_ep *r = &ane->ep[ANE_T6021_EP_INIT];
	u32 cursor;
	u64 doorbell;
	int err;

	BUILD_BUG_ON(ANE_T6021_EP_INIT != 1);

	if (!cmd || size < sizeof(struct ane_csne_hdr))
		return -EINVAL;
	if (size > ANE_CSNE_CMD_MAX_SIZE)
		return -E2BIG;
	if (!r->started)
		return -ENOTCONN;

	mutex_lock(&ane->mbox_lock);

	/* K14 rtbuddyEndpointSendMessage 0x…95f3af8-c04: size bound
	 * first, then the write cursor — kept when cursor+size fits
	 * strictly below ring_size, else the slot restarts at 0 (an
	 * exact fit wraps too: csel lo @0x…95f3b04). */
	if (size > r->ring_size) {
		err = -E2BIG;
		goto out;
	}
	cursor = r->write_cursor;
	if (cursor + size >= r->ring_size)
		cursor = 0;

	/* Ring slot is device-visible DMA memory; the MBI doorbell write
	 * (dma_wmb before it) orders this copy. */
	memcpy(r->ring + cursor, cmd, size);

	doorbell = ane_ep_doorbell_encode(cursor, size);
	(void)doorbell;
	/* INIT-channel doorbell bit is unknown until the MBI table dump
	 * pins it — refuse rather than ring a guessed bit. */
	dev_err_once(ane->dev,
		     "csne submit refused: INIT doorbell bit unpinned (MBI table first)\n");
	err = -EOPNOTSUPP;
out:
	mutex_unlock(&ane->mbox_lock);
	return err;
}

irqreturn_t ane_t6021_rtkit_irq_thread(int irq, void *data)
{
	ane_t6021_rtkit_drain(data);
	return IRQ_HANDLED;
}

int ane_t6021_rtkit_init(struct ane_t6021 *ane)
{
	static const u32 sizes[] = { 0x10000, 0x20000, 0x40000 };
	int id, err;

	/* doorbell codec round-trip, unit-1 (4K) size class — the only
	 * class the RTBuddy ring sizes produce; then a non-multiple
	 * size to pin the ceiling rounding (K14 cinc @0x…95fe8dc) */
	for (id = 0; id < ARRAY_SIZE(sizes); id++) {
		u64 msg = ane_ep_doorbell_encode(0x1234, sizes[id]);

		if (ane_ep_doorbell_size(msg) != sizes[id] ||
		    FIELD_GET(ANE_EP_DOORBELL_OFFSET, msg) != 0x1234) {
			dev_err(ane->dev, "doorbell codec broken (size %#x)\n",
				sizes[id]);
			return -EINVAL;
		}
	}
	if (ane_ep_doorbell_size(ane_ep_doorbell_encode(0, 0x1234)) !=
	    0x2000) {
		dev_err(ane->dev, "doorbell codec ceiling broken\n");
		return -EINVAL;
	}

	mutex_init(&ane->mbox_lock);
	memcpy(ane->ep, ane_ep_config, sizeof(ane->ep));

	/* RTBuddy app rings up front: STARTEP for ids 1..6 fires from the
	 * MGMT path once the fw ACKs the AP power state (W2 §3, §7). */
	for (id = ANE_T6021_EP_INIT; id < ANE_T6021_EP_COUNT; id++) {
		struct ane_t6021_ep *r = &ane->ep[id];

		r->ring = dma_alloc_coherent(ane->dev, r->ring_size,
					     &r->ring_iova, GFP_KERNEL);
		if (!r->ring) {
			err = -ENOMEM;
			goto free;
		}
		dev_dbg(ane->dev, "ep%u %s ring %u bytes at %pad\n",
			r->id, r->name, r->ring_size, &r->ring_iova);
	}
	return 0;

free:
	ane_t6021_rtkit_shutdown(ane);
	return err;
}

void ane_t6021_rtkit_shutdown(struct ane_t6021 *ane)
{
	int id;

	for (id = ANE_T6021_EP_INIT; id < ANE_T6021_EP_COUNT; id++) {
		struct ane_t6021_ep *r = &ane->ep[id];

		if (!r->ring)
			continue;
		dma_free_coherent(ane->dev, r->ring_size, r->ring,
				  r->ring_iova);
		r->ring = NULL;
		r->started = false;
	}
	mutex_destroy(&ane->mbox_lock);
}
