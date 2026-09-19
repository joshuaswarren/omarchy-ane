// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* T6021 ANE RTKit mailbox core — ASC mailbox ops, MGMT handshake,
 * RTBuddy app-endpoint rings.
 *
 * C port of omarchy-ane rtkit/h14_rtkit_hello.py (commit 6ad26b7) with
 * the u64 message halves the W1-prep fix pinned: msg0 rides SEND0/RECV0
 * as a full 64-bit register (written/read as two 32-bit halves would
 * truncate the MGMT type at bits [59:52]); msg1 (endpoint) rides
 * SEND1/RECV1. FULL/EMPTY semantics and the 1-deep FIFO behaviour
 * follow drivers/soc/apple/mailbox.c; MGMT encodings follow
 * drivers/soc/apple/rtkit.c. Nothing here is live-validated yet — W1's
 * first exchange is still walled (receipt 2026-09-19-h14-w1-first-rpc).
 */

#include <linux/bitmap.h>
#include <linux/dev_printk.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/minmax.h>

#include <asm/barrier.h>

#include "ane_t6021.h"

/* ---- mailbox (apple-mailbox.c ASC variant, ANE-block-relative) ---- */

static int ane_mbox_send(struct ane_t6021 *ane, u64 msg0, u8 ep)
{
	void __iomem *regs = ane->base[ANE_T6021_REG_ENGINE];
	u32 ctrl;
	int err;

	/* m1n1 Mbox.send: never overwrite an in-flight slot (1-deep FIFO) */
	err = readl_poll_timeout(regs + ANE_MBOX_A2I_CONTROL, ctrl,
				 !(ctrl & ANE_MBOX_CONTROL_FULL),
				 100, ANE_MBOX_TX_TIMEOUT * 1000);
	if (err)
		return err;

	writeq_relaxed(msg0, regs + ANE_MBOX_A2I_SEND0);
	dma_wmb();
	writel_relaxed(ep, regs + ANE_MBOX_A2I_SEND1);
	return 0;
}

static bool ane_mbox_recv(struct ane_t6021 *ane, u64 *msg0, u8 *ep)
{
	void __iomem *regs = ane->base[ANE_T6021_REG_ENGINE];
	u32 ctrl = readl_relaxed(regs + ANE_MBOX_I2A_CONTROL);

	if (ctrl & ANE_MBOX_CONTROL_EMPTY)
		return false;

	*msg0 = readq_relaxed(regs + ANE_MBOX_I2A_RECV0);
	*ep = readl_relaxed(regs + ANE_MBOX_I2A_RECV1) & 0xff;
	return true;
}

static void ane_rtkit_mgmt_send(struct ane_t6021 *ane, unsigned int type,
				u64 payload)
{
	u64 msg0 = FIELD_PREP(ANE_RTKIT_TYPE, type) | payload;
	int err;

	err = ane_mbox_send(ane, msg0, 0);
	if (err)
		dev_err(ane->dev, "mgmt send type %#x: %d\n", type, err);
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

/* W4: submission path on the INIT channel. Endpoint-open only here. */
int ane_t6021_csne_submit(struct ane_t6021 *ane, const void *cmd, size_t size)
{
	dev_warn_once(ane->dev,
		      "CSNE_CMD submission is W4 work — not implemented\n");
	return -EOPNOTSUPP;
}

static void ane_rtkit_start_ep(struct ane_t6021 *ane, u8 ep)
{
	/* FLAG-only STARTEP, exactly as staged in h14_rtkit_hello.py.
	 * [INFERENCE, open for W1/W4] how the fw learns each app ring's
	 * DART address: the kext RTBuddy records carry it somewhere this
	 * decode has not pinned (RTKit-standard STARTEP buffer field
	 * (iova>>12 | log2sz<<48) is the candidate); ring_size and the
	 * per-EP fourcc are config-table proven, the IOVA handover is not
	 * proven. W4 pins it against the live exchange. */
	ane_rtkit_mgmt_send(ane, ANE_RTKIT_MGMT_STARTEP,
			    FIELD_PREP(ANE_RTKIT_STARTEP_EP, ep) |
			    ANE_RTKIT_STARTEP_FLAG);
}

static void ane_rtkit_start_app_eps(struct ane_t6021 *ane)
{
	int id;

	for (id = ANE_T6021_EP_INIT; id < ANE_T6021_EP_COUNT; id++) {
		if (!test_bit(id, ane->announced))
			continue;
		ane_rtkit_start_ep(ane, id);
		ane->ep[id].started = true;
	}
}

/* rtkit.c system endpoints, started when the fw announces them */
static const unsigned int ane_sys_eps[] = {
	ANE_RTKIT_EP_CRASHLOG, ANE_RTKIT_EP_SYSLOG,
	ANE_RTKIT_EP_DEBUG, ANE_RTKIT_EP_IOREPORT,
	ANE_RTKIT_EP_OSLOG, ANE_RTKIT_EP_TRACEKIT
};

/* fw->host doorbell: decode, log, count. Capture-only — the T2F/HT
 * ring payload walk is W4 (EP2/EP3 deliver fw->host commands, EP6 is
 * host-polled; W2 §3). */
static void ane_rtkit_app_doorbell(struct ane_t6021 *ane, u8 ep, u64 msg0)
{
	struct ane_t6021_ep *r = &ane->ep[ep];
	/* offset is a 44-bit field: bound check in u64 so a garbled word
	 * cannot wrap through the u32 ring size (kext checks the same
	 * bound on the wide type, HandleRTBuddyMessage 0x…95ff128) */
	u64 offset = FIELD_GET(ANE_EP_DOORBELL_OFFSET, msg0);
	u64 size = ane_ep_doorbell_size(msg0);

	if (offset + size > r->ring_size) {
		dev_err_ratelimited(ane->dev,
				    "ep%u %s: doorbell out of ring: off %#llx size %#llx\n",
				    ep, r->name, offset, size);
		return;
	}
	dev_dbg(ane->dev, "ep%u %s doorbell: off %#llx size %#llx\n",
		ep, r->name, offset, size);
}

/* ---- MGMT receive dispatch (mirrors h14_rtkit_hello.py main loop) ---- */

static void ane_rtkit_rx_mgmt(struct ane_t6021 *ane, u64 msg0)
{
	unsigned int type = FIELD_GET(ANE_RTKIT_TYPE, msg0);
	unsigned long bitmap;
	u32 base, state;
	u8 id;

	switch (type) {
	case ANE_RTKIT_MGMT_HELLO: {
		unsigned int ver_min = FIELD_GET(ANE_RTKIT_HELLO_MINVER, msg0);
		unsigned int ver_max = FIELD_GET(ANE_RTKIT_HELLO_MAXVER, msg0);
		unsigned int want = min((unsigned int)ANE_RTKIT_VER_MAX,
					ver_max);

		dev_info(ane->dev, "RTKit HELLO: fw supports %u..%u, want %u\n",
			 ver_min, ver_max, want);
		ane_rtkit_mgmt_send(ane, ANE_RTKIT_MGMT_HELLO_REPLY,
				    FIELD_PREP(ANE_RTKIT_HELLO_MINVER, want) |
				    FIELD_PREP(ANE_RTKIT_HELLO_MAXVER, want));
		break;
	}

	case ANE_RTKIT_MGMT_EPMAP:
		base = FIELD_GET(ANE_RTKIT_EPMAP_BASE, msg0);
		bitmap = FIELD_GET(ANE_RTKIT_EPMAP_BITMAP, msg0);
		for_each_set_bit(id, &bitmap, 32)
			set_bit(32 * base + id, ane->announced);
		/* rtkit.c reply shape: LAST echo when the fw said LAST,
		 * MORE (bit 0) otherwise — never an empty reply */
		ane_rtkit_mgmt_send(ane, ANE_RTKIT_MGMT_EPMAP,
				    FIELD_PREP(ANE_RTKIT_EPMAP_BASE, base) |
				    ((msg0 & ANE_RTKIT_EPMAP_LAST) ?
				     ANE_RTKIT_EPMAP_LAST :
				     ANE_RTKIT_EPMAP_REPLY_MORE));
		if (msg0 & ANE_RTKIT_EPMAP_LAST)
			for (unsigned int i = 0;
			     i < ARRAY_SIZE(ane_sys_eps); i++)
				if (test_bit(ane_sys_eps[i], ane->announced))
					ane_rtkit_start_ep(ane,
							   ane_sys_eps[i]);
		break;

	case ANE_RTKIT_MGMT_SET_IOP_PWR_STATE:
		/* selene initiates; host echoes ACK (kext/py shape — the
		 * host-initiated boot() of stock rtkit.c is not what this
		 * fw does after iBoot bring-up) */
		state = FIELD_GET(ANE_RTKIT_PWR_STATE, msg0);
		ane_rtkit_mgmt_send(ane,
				    ANE_RTKIT_MGMT_SET_IOP_PWR_STATE_ACK,
				    FIELD_PREP(ANE_RTKIT_PWR_STATE, state));
		break;

	case ANE_RTKIT_MGMT_SET_AP_PWR_STATE_ACK:
		/* same code both directions (py: 0xb/0xb); incoming = the
		 * fw's ACK: app firmware is live, open the app EPs */
		state = FIELD_GET(ANE_RTKIT_PWR_STATE, msg0);
		dev_info(ane->dev, "RTKit AP power state %#x acked\n", state);
		ane_rtkit_start_app_eps(ane);
		ane->booted = true;
		break;

	default:
		dev_dbg(ane->dev, "unhandled MGMT type %#x msg %#llx\n",
			type, msg0);
	}
}

void ane_t6021_rtkit_drain(struct ane_t6021 *ane)
{
	u64 msg0;
	u8 ep;

	mutex_lock(&ane->mbox_lock);
	while (ane_mbox_recv(ane, &msg0, &ep)) {
		if (ep == 0) {
			ane_rtkit_rx_mgmt(ane, msg0);
			continue;
		}
		if (ep >= ANE_T6021_EP_INIT && ep < ANE_T6021_EP_COUNT)
			ane_rtkit_app_doorbell(ane, ep, msg0);
		else
			dev_dbg(ane->dev, "message on unknown ep %u\n", ep);
	}
	mutex_unlock(&ane->mbox_lock);
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
	 * class the RTBuddy ring sizes produce */
	for (id = 0; id < ARRAY_SIZE(sizes); id++) {
		u64 msg = ane_ep_doorbell_encode(0x1234, sizes[id]);

		if (ane_ep_doorbell_size(msg) != sizes[id] ||
		    FIELD_GET(ANE_EP_DOORBELL_OFFSET, msg) != 0x1234) {
			dev_err(ane->dev, "doorbell codec broken (size %#x)\n",
				sizes[id]);
			return -EINVAL;
		}
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
