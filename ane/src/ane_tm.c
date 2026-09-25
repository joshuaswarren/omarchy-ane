// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2022 Eileen Yoon <eyn@gmx.com> */

#include <linux/device.h>
#include <linux/iopoll.h>
#include <linux/pm_runtime.h>
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

/* The ane SET block (m1n1 ANE.ps_map) maps the pmgr power-state words
 * for this engine (set0, base, set1..4). pmgr reads are always safe and
 * give recovery its ACTUAL power evidence; the SET words themselves are
 * firmware-locked: a direct write external-aborts the SoC. Bisect
 * evidence, 2026-09-16: the T6001 kill is netconsole-named at
 * 0x28e08c000 (PS_SET0 down, all sets gated); T8103 hard-reset the same
 * way at 0x23b70c000 with the 95dbcf3-era gate armed. No code here ever
 * writes the SET block. */
#define ANE_PS_ACTUAL_MASK	  0xf0
#define ANE_PS_WORDS		  6 /* set0, base, set1..4 */
#define ANE_PS_ALL_ON		  ((1U << (4 * ANE_PS_WORDS)) - 1)

/* ACTUAL nibble of each SET word, word 0 in the low nibble; 0 when the
 * SET block is unmapped. pmgr registers only: engine MMIO is never
 * read for power state (a readl through a warm gate external-aborts
 * and hard-resets the machine). */
u32 ane_ps_act(struct ane_device *ane)
{
	u32 v = 0;
	int i;

	if (!ane->ps)
		return 0;
	for (i = 0; i < ANE_PS_WORDS; i++)
		v |= ((readl(ane->ps + i * 8) & ANE_PS_ACTUAL_MASK) >> 4)
		     << (i * 4);
	return v;
}

/* Bisect probe: per-word named reads of the SET window. Each word logs
 * before its readl, so a window whose address does not decode on this
 * SoC is named by the last off-box line (word index + byte offset)
 * instead of a silent hard reset between two other prints. */
u32 ane_ps_act_probe(struct ane_device *ane)
{
	u32 v = 0;
	int i;

	if (!ane->ps) {
		dev_info(ane->dev, "ps probe: SET window unmapped\n");
		return 0;
	}
	dev_info(ane->dev, "ps probe: SET window at %pap, %d words\n",
		 &ane->ps_base, ANE_PS_WORDS);
	for (i = 0; i < ANE_PS_WORDS; i++) {
		u32 w;

		dev_info(ane->dev, "ps probe: word %d @ ps+0x%02x reading\n",
			 i, i * 8);
		w = readl(ane->ps + i * 8);
		v |= ((w & ANE_PS_ACTUAL_MASK) >> 4) << (i * 4);
		dev_info(ane->dev, "ps probe: word %d -> %#x\n", i, w);
	}
	return v;
}

/* Recovery-path MMIO logging. Every write prints before and after, and
 * every engine read prints its value, each line carrying the pmgr
 * ACTUAL of the owning partitions. A write that external-aborts the
 * SoC is then named by the last line the off-box netconsole carried,
 * and the bisect starts from that register instead of a guess. */
static void ane_rec_writel(struct ane_device *ane, const char *reg,
			   void __iomem *addr, u32 val)
{
	dev_info(ane->dev, "ANEWR %s <- %#x (ps act %#x)\n", reg, val,
		 ane_ps_act(ane));
	writel(val, addr);
	dev_info(ane->dev, "ANEWR %s wrote (ps act %#x)\n", reg,
		 ane_ps_act(ane));
}

static u32 ane_rec_read32(struct ane_device *ane, const char *reg,
			  void __iomem *addr)
{
	u32 val = readl(addr);

	dev_info(ane->dev, "ANERD %s -> %#x (ps act %#x)\n", reg, val,
		 ane_ps_act(ane));
	return val;
}

void ane_tm_enable(struct ane_device *ane, bool rec)
{
	void __iomem *tq_en = ane->engine + ANE_TM_BASE + TM_TQ_EN;
	u32 val = rec ? ane_rec_read32(ane, "TM_TQ_EN tm+0x0c", tq_en)
		      : readl(tq_en);
	char reg[24];

	val |= 0x1000;
	if (rec)
		ane_rec_writel(ane, "TM_TQ_EN tm+0x0c", tq_en, val);
	else
		writel(val, tq_en);

	for (int qid = 0; qid < ANE_TQ_COUNT; qid++) {
		void __iomem *prty =
			ane->engine + ANE_TQ_BASE + TQ_PRTY(qid);

		if (rec) {
			snprintf(reg, sizeof(reg), "TQ_PRTY[%d] tq+%#x", qid,
				 TQ_PRTY(qid));
			ane_rec_writel(ane, reg, prty, TQ_PRTY_TABLE[qid]);
		} else {
			writel(TQ_PRTY_TABLE[qid], prty);
		}
	}

	if (rec) {
		ane_rec_writel(ane, "TM_IRQ_EN1 tm+0x68",
			       ane->engine + ANE_TM_BASE + TM_IRQ_EN1,
			       0x4000000);
		ane_rec_writel(ane, "TM_IRQ_EN2 tm+0x70",
			       ane->engine + ANE_TM_BASE + TM_IRQ_EN2, 0x6);
	} else {
		tm_write32(ane, TM_IRQ_EN1, 0x4000000);
		tm_write32(ane, TM_IRQ_EN2, 0x6);
	}
}

u32 ane_tm_status(struct ane_device *ane)
{
	return tm_read32(ane, TM_STATUS);
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
			"tm completion failed: %d, finish lines=%x (q%d nid=%#x)\n",
			err, finished, req->qid, req->nid);
	}

	/* One bounded recovery attempt: stop the tm, power-cycle the engine
	 * and return to accepting work. Only a failed reset preserves
	 * resources until reboot. */
	if (ane_tm_recover(ane) < 0)
		dev_err(ane->dev,
			"recovery failed; preserving resources until reboot\n");
	return err;
}

/*
 * Bounded recovery after a failed task. The only reset this driver knows
 * is the partition power cycle system sleep already performs on this
 * hardware. With the several ANE power partitions attached as genpd
 * devices (T8103 five, T6001 similar), gating happens on those devices;
 * with a single domain attached directly to the device, on the device
 * itself. Gating the partitions stops any DMA still fetching the dead
 * task and clears the tm register file; ungating brings the engine back
 * through the probe resume path (ane_tm_enable), then the task manager
 * must report idle. BO mappings deliberately stay in place: they remain
 * coherent through the cycle and valid for the next submit, and once the
 * wedge clears, BO_FREE unmaps them through the normal path again.
 * Callers hold engine_lock.
 *
 * T6001 note, bisect evidence 2026-09-16: its set0/base islands are
 * firmware-locked (direct pmgr writes external-abort; see the SET block
 * comment above) and hold the tm/tq file in retention through any
 * genpd cycle - TQ_EN still reads 0x3000 after a 20 ms and a 2 s gate,
 * and tasks dispatched afterwards complete but compute nondeterministic
 * garbage. The idle-or-fresh poll below therefore fails on T6001 and
 * recovery preserves until reboot instead of serving wrong data.
 */

/* No engine MMIO unless the owning partitions read powered on (ACTUAL
 * nibble per word): a readl through a warm gate external-aborts and
 * hard resets the machine. Covers set0, base and set1..4 from the SET
 * block map; sys_cpu rides its own genpd resume and has no cell in
 * this block on T6001. */
static int ane_ps_verify_on(struct ane_device *ane)
{
	u32 act = 0;
	int err = -ETIMEDOUT;
	int i;

	if (!ane->ps)
		return 0;
	for (i = 0; i < 100; i++) {
		act = ane_ps_act(ane);
		if (act == ANE_PS_ALL_ON) {
			err = 0;
			break;
		}
		usleep_range(1000, 2000);
	}
	dev_info(ane->dev, "ANERD ps verify act=%#x err=%d\n", act, err);
	return err;
}

static int ane_pd_cycle(struct ane_device *ane)
{
	int err = 0;

	ane->recovering = true;
	if (ane->pd_count > 1) {
		int gated = 0;

		for (int i = 0; i < ane->pd_count; i++) {
			pm_runtime_get_noresume(ane->pd_dev[i]);
			gated = i + 1;
			dev_info(ane->dev,
				 "ANERD pd[%d] %s force_suspend begin (ps act %#x)\n",
				 i, dev_name(ane->pd_dev[i]), ane_ps_act(ane));
			err = pm_runtime_force_suspend(ane->pd_dev[i]);
			dev_info(ane->dev,
				 "ANERD pd[%d] force_suspend -> %d (ps act %#x)\n",
				 i, err, ane_ps_act(ane));
			if (err)
				break;
		}
		if (!err)
			for (int i = 0; i < ane->pd_count; i++) {
				dev_info(ane->dev,
					 "ANERD pd[%d] %s force_resume begin (ps act %#x)\n",
					 i, dev_name(ane->pd_dev[i]),
					 ane_ps_act(ane));
				err = pm_runtime_force_resume(ane->pd_dev[i]);
				dev_info(ane->dev,
					 "ANERD pd[%d] force_resume -> %d (ps act %#x)\n",
					 i, err, ane_ps_act(ane));
				if (err)
					break;
			}
		while (gated--)
			pm_runtime_put_noidle(ane->pd_dev[gated]);
	} else {
		/* Pin a second usage ref for the cycle: force_suspend only
		 * marks needs_force_resume when a ref beyond the probe one
		 * is held, and without that mark force_resume would leave
		 * the partition gated. */
		pm_runtime_get_noresume(ane->dev);
		dev_info(ane->dev, "ANERD dev force_suspend begin\n");
		err = pm_runtime_force_suspend(ane->dev);
		dev_info(ane->dev, "ANERD dev force_suspend -> %d\n", err);
		if (!err) {
			dev_info(ane->dev, "ANERD dev force_resume begin\n");
			err = pm_runtime_force_resume(ane->dev);
			dev_info(ane->dev, "ANERD dev force_resume -> %d\n",
				 err);
		}
		pm_runtime_put_noidle(ane->dev);
	}
	ane->recovering = false;

	return err;
}

int ane_tm_recover(struct ane_device *ane)
{
	u32 status;
	int err;

	lockdep_assert_held(&ane->engine_lock);
	if (!atomic_read(&ane->wedged))
		return 0;

	dev_err(ane->dev, "recovering: power-cycling engine partitions\n");

	err = ane_pd_cycle(ane);
	if (err) {
		dev_err(ane->dev, "recovery: engine power cycle failed: %d\n",
			err);
		return err;
	}

	/* No engine MMIO before the islands read powered on. */
	err = ane_ps_verify_on(ane);
	if (err) {
		dev_err(ane->dev,
			"recovery: ane set islands not powered on: %d\n", err);
		return err;
	}

	/* Power-on reset cleared the tm register file; re-arm it exactly
	 * like the probe resume path does. */
	ane_tm_enable(ane, true);

	err = readl_poll_timeout(ane->engine + ANE_TM_BASE + TM_STATUS,
				 status,
				 (status & TM_IS_IDLE) ||
				 status == ane->tm_status_fresh,
				 100, 1000000);
	status = ane_rec_read32(ane, "TM_STATUS tm+0x54",
				ane->engine + ANE_TM_BASE + TM_STATUS);
	if (err) {
		dev_err(ane->dev, "recovery: tm not idle after reset: %#x\n",
			status);
		return err;
	}

	if (atomic_xchg(&ane->wedged, 0)) {
		module_put(THIS_MODULE); /* drop the wedge pin */
		dev_info(ane->dev, "tm recovered: idle, accepting work again\n");
	}
	return 0;
}

/*
 * Drop the wedge pin on demand. The recovery path in ane_tm_recover
 * does an atomic_xchg + module_put, but only on its success branch -
 * a real -110 with the engine stuck leaves ane->wedged set and the
 * module refcount one too high, so rmmod fails forever and the bug
 * becomes a kernel-brick-without-reboot. ane_drm_postclose calls
 * this unconditionally so every session close releases the pin,
 * letting the operator unload the ko for an updated build even when
 * the engine itself cannot be cleared in software.
 *
 * Pair count: must follow an ane_wedge_set() that called
 * __module_get(THIS_MODULE). Today the only such set is in the wedge:
 * branch of ane_tm_execute(); future sets must call this in their
 * clean-up path too.
 */
void ane_wedge_clear(struct ane_device *ane)
{
	if (atomic_xchg(&ane->wedged, 0)) {
		module_put(THIS_MODULE);
		dev_info(ane->dev,
			 "wedge pin released (engine state unknown)\n");
	}
}
