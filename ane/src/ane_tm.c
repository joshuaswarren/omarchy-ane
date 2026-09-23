// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2022 Eileen Yoon <eyn@gmx.com> */

#include <linux/device.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include "ane_tm.h"

#define ANE_TQ_COUNT 8
static const int TQ_PRTY_TABLE[ANE_TQ_COUNT] = { 0x1, 0x2, 0x3,	 0x4,
						 0x5, 0x6, 0x1e, 0x1f };

#define ANE_TM_BASE		  0x20000
#define ANE_TQ_BASE		  0x21000

#define TM_ADDR			  0x0
#define TM_INFO			  0x4
#define TM_PUSH			  0x8
#define TM_TQ_EN		  0xc

#define TM_IRQ_EVTC(line)	  (0x14 + (line * 0x14))
#define TM_IRQ_INFO(line)	  (0x18 + (line * 0x14))
#define TM_IRQ_UNK1(line)	  (0x1c + (line * 0x14))
#define TM_IRQ_TMST(line)	  (0x20 + (line * 0x14))
#define TM_IRQ_UNK2(line)	  (0x24 + (line * 0x14))

#define TM_COMMITTED		  0x44
#define TM_STATUS		  0x54
#define TM_ERROR1		  0x58
#define TM_ERROR2		  0x5c
#define TM_ERROR3		  0x60
#define TM_IRQ_EN1		  0x68
#define TM_IRQ_ACK		  0x6c
#define TM_IRQ_EN2		  0x70

#define TQ_STATUS(qid)		  (0x000 + (qid * 0x148))
#define TQ_PRTY(qid)		  (0x010 + (qid * 0x148))
#define TQ_VACANT(qid)		  (0x014 + (qid * 0x148))
#define TQ_INFO(qid)		  (0x01c + (qid * 0x148))

#define TQ_BAR1(qid, bdx)	  (0x020 + (qid * 0x148) + (bdx * 0x4))
#define TQ_NID1(qid)		  (0x0a0 + (qid * 0x148))
#define TQ_SIZE2(qid)		  (0x0a4 + (qid * 0x148))
#define TQ_ADDR2(qid)		  (0x0a8 + (qid * 0x148))

#define TQ_BAR2(qid, bdx)	  (0x0ac + (qid * 0x148) + (bdx * 0x4))
#define TQ_NID2(qid)		  (0x12c + (qid * 0x148))
#define TQ_SIZE1(qid)		  (0x130 + (qid * 0x148))
#define TQ_ADDR1(qid)		  (0x134 + (qid * 0x148))

#define TM_IS_IDLE		  0x1
#define TM_IS_FINE		  0x22222222

#define tm_read32(ane, off)	  (readl(ane->engine + ANE_TM_BASE + off))
#define tq_read32(ane, off)	  (readl(ane->engine + ANE_TQ_BASE + off))
#define tm_write32(ane, off, val) (writel(val, ane->engine + ANE_TM_BASE + off))
#define tq_write32(ane, off, val) (writel(val, ane->engine + ANE_TQ_BASE + off))

void ane_tm_enable(struct ane_device *ane)
{
	tm_write32(ane, TM_TQ_EN, tm_read32(ane, TM_TQ_EN) | 0x1000);

	for (int qid = 0; qid < ANE_TQ_COUNT; qid++) {
		tq_write32(ane, TQ_PRTY(qid), TQ_PRTY_TABLE[qid]);
	}

	tm_write32(ane, TM_IRQ_EN1, 0x4000000);
	tm_write32(ane, TM_IRQ_EN2, 0x6);
}

int ane_tm_enqueue(struct ane_device *ane, struct ane_request *req)
{
	int qid = req->qid;

	tq_write32(ane, TQ_STATUS(qid), 0x1);

	for (int bdx = 0; bdx < ANE_TILE_COUNT; bdx++) {
		tq_write32(ane, TQ_BAR1(qid, bdx), req->bar[bdx]);
	}

	tq_write32(ane, TQ_SIZE1(qid), ((req->td_size >> 2) - 1) << 0x10);
	tq_write32(ane, TQ_ADDR1(qid), req->btsp_iova);
	tq_write32(ane, TQ_NID1(qid), (req->nid & 0xff) << 8 | 1);

	return 0;
}

static void ane_tm_push_tq(struct ane_device *ane, struct ane_request *req)
{
	int qid = req->qid;
	tm_write32(ane, TM_ADDR, tq_read32(ane, TQ_ADDR1(qid)));
	tm_write32(ane, TM_INFO, tq_read32(ane, TQ_SIZE1(qid)) | req->td_count);
	tm_write32(ane, TM_PUSH, TQ_PRTY_TABLE[qid] | (qid & 7) << 8); // magic
}

static int ane_tm_collect_events(struct ane_device *ane,
				 struct ane_request *req, u32 *finished)
{
	for (unsigned int line = 0; line < 2; line++) {
		u32 count = tm_read32(ane, TM_IRQ_EVTC(line));

		if (count > 64)
			return -EIO;
		for (u32 n = 0; n < count; n++) {
			u32 info = tm_read32(ane, TM_IRQ_INFO(line));

			tm_read32(ane, TM_IRQ_UNK1(line));
			tm_read32(ane, TM_IRQ_TMST(line));
			tm_read32(ane, TM_IRQ_UNK2(line));
			if (req && info == (0x05000000 | (req->nid << 16) |
					    (req->td_count - 1)))
				*finished |= BIT(line);
		}
	}
	tm_write32(ane, TM_IRQ_ACK, tm_read32(ane, TM_IRQ_ACK) | 2);
	return *finished == 3 && (tm_read32(ane, TM_STATUS) & TM_IS_IDLE);
}

/* read_poll_timeout op: consult the DART latch before any engine access.
 * Returns 1 to end the poll when a fault is latched; the caller then
 * handles the fault without ever reading the wedged engine. */
static int ane_tm_poll_step(struct ane_device *ane, struct ane_request *req,
			    u32 *finished)
{
	if (ane_dart_faulted(ane, NULL, NULL))
		return 1;
	return ane_tm_collect_events(ane, req, finished);
}

int ane_tm_execute(struct ane_device *ane, struct ane_request *req)
{
	u32 finished = 0;
	u32 fault_status = 0;
	u64 fault_iova = 0;
	struct ane_dart_scratch scratch[ANE_DART_SCRATCH_MAX];
	int scratch_pages = 0;
	bool faulted = false;
	int err, status;

	lockdep_assert_held(&ane->engine_lock);
	ane_dart_mask(ane);

	err = ane_tm_collect_events(ane, NULL, &finished);
	if (err < 0)
		goto wedge;
	wmb();
	ane_tm_push_tq(ane, req);

	for (;;) {
		/* Latch-first completion poll. Engine register reads hang
		 * jwm1 once a DART fault has landed (lockups 1 and 3: the
		 * journal ends inside the 1-microsecond completion poll,
		 * before any fault handling ran), so the poll op consults
		 * the DART error latch first and stops the poll before the
		 * engine is touched. Residual hazard: a fault landing
		 * between one iteration's latch read and engine read. */
		err = read_poll_timeout(ane_tm_poll_step, status,
					status != 0, 1, 1000000, false,
					ane, req, &finished);
		if (!err && status < 0)
			err = status;
		if (ane_dart_faulted(ane, &fault_iova, &fault_status)) {
			faulted = true;
			if (scratch_pages >= ANE_DART_SCRATCH_MAX)
				break;
			if (ane_dart_drain_fault(ane, fault_iova, scratch,
						 &scratch_pages))
				break;
			/* The retried transaction now completes; poll on. */
			continue;
		}
		break;
	}

	if (faulted) {
		/* The faulting program drained through the scratch pages
		 * and the engine is idle again (the latch is clear or the
		 * poll would still be running). Gate the queue with the
		 * normal completion writes, hand the scratch pages back,
		 * and fail this request. No partition power cycle, no
		 * stream disable: the page tables were never changed by
		 * the fault. */
		if (err || ane_dart_faulted(ane, NULL, NULL)) {
			/* Drain failed or the drained program never reached
			 * completion: the engine is not idle and access is
			 * off the table. wedge releases the scratch. */
			err = err ?: -EIO;
			goto wedge;
		}
		status = tq_read32(ane, TQ_NID1(req->qid));
		tq_write32(ane, TQ_NID1(req->qid), status & ~1U);
		rmb();
		tq_write32(ane, TQ_STATUS(req->qid), 0x0);
		ane_dart_release_scratch(ane, scratch, scratch_pages);
		ane_dart_unmask(ane);
		dev_err(ane->dev,
			"DART fault contained: status=%#x iova=%#llx scratch=%d\n",
			fault_status, fault_iova, scratch_pages);
		return -EIO;
	}
	if (err)
		goto wedge;
	status = tq_read32(ane, TQ_NID1(req->qid));
	if (((status >> 8) & 0xff) != req->nid) {
		err = -EIO;
		goto wedge;
	}
	tq_write32(ane, TQ_NID1(req->qid), status & ~1U);
	rmb();
	tq_write32(ane, TQ_STATUS(req->qid), 0x0);
	ane_dart_unmask(ane);
	return 0;

wedge:
	ane_dart_release_scratch(ane, scratch, scratch_pages);
	ane_dart_unmask(ane);
	if (atomic_xchg(&ane->wedged, 1) == 0) {
		__module_get(THIS_MODULE);
		dev_err(ane->dev,
			"tm completion failed: %d, finish lines=%x; preserving resources until reboot\n",
			err, finished);
	}
	return err;
}
