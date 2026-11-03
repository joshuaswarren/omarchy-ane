// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ANE clock level (T6001, M1 Max): macOS raises the ANE clock through the PMP and the
 * pmgr DVFS block; Linux never did, so every ANE job ran at the lowest level (level 0).
 * Measured on the 16-inch (T6001) under Linux, receipt receipts/2026-10-24-ane-level-driver:
 *   - level 1 cuts the busy time per job of a bandwidth-bound rung by 40 percent
 *     (1.07 -> 0.65 ms) and a full Qwen decode step by 42 percent (0.2009 -> 0.1158 s,
 *     4.98 -> 8.64 tokens per second); the logits are bit-identical at every level;
 *   - level 2 adds under 3 percent (the program is then memory-bound), levels above
 *     2 were written by the first version. The default became level 5 on 2026-10-30 after a 30 minute soak (see the dvfs_level parameter below).
 *
 * What the PMP wants, in the order macOS writes it (a macOS 26.6.2 trace of three ANE power cycles),
 * and in the order the receipt's runs used with the ANE already powered:
 *   up:   DVFS_ON := 1; DVFS_CMD := 0x80000000; PS-REQ bit ANE_SYS set (wait for PS-ACK);
 *         DVFS_CMD := 0x80000001; level word := 0x2000000000000001
 *   down: DVFS_CMD := 0x80000010; level word := 0x2000000000000000; PS-REQ bit cleared;
 *         DVFS_CMD := 0x80000000; DVFS_ON := 0
 * DVFS_ON and DVFS_CMD live in the pmgr block named RegMap 113 in the shipped system (ADT reg[39], PA 0x285868000,
 * DVFS_ON at +0x2000, DVFS_CMD at +0xa00). The PS-REQ word and the level word live in the PMP PTD
 * window (read entities at +reg*16, update space at +0x10000 + reg*8; PS-REQ reg 0xf8, PS-ACK
 * reg 0x100, PMP-STATUS reg 1, ANE level reg 0x10c).
 *
 * Evidence for every address, offset and word above (kernelcache and trace hashes, run logs): receipts/2026-10-24-ane-level-driver/README.md,
 * section Evidence; that receipt and this file were first committed together in 2bfb216.
 *
 * Safety:
 *  - Optional. The DT must name both windows (reg-names "dvfs" and "ptd"); without them the
 *    driver behaves as before. Level control needs the PMP firmware running (PMP-STATUS reads 1)
 *    and the ANE partition up; otherwise it stays at level 0 and says so in sysfs perf_state.
 *  - The PTD window sits in the PMS SRAM power domain, which the DT lists as an ANE power domain,
 *    so it is powered whenever this runs. All access is ioremap_np (of_iomap takes the bus's
 *    nonposted-mmio into account); a posted mapping took an SError in the first experiments.
 *  - The writes are exactly the words above: DVFS_ON 0/1, DVFS_CMD 0x80000000/1/10, the ANE_SYS
 *    bit of PS-REQ and the level words 0 and 1. The up sequence starts only when the hardware words
 *    are the ones the proving runs saw (PS-REQ with bit 10 clear, level word 0 or a level word, DVFS_ON 0,
 *    PMP-STATUS 1) and the PS-REQ readback after the ACK has bit 10 set; any other value leaves level 0.
 *    The other bits of PS-REQ belong to other PMP clients (display, ISP, video decode, USB4 and the AGX vote, bit 9): they are
 *    neither checked nor changed, only bit 10 is requested (ane_dvfs_req.h). When pmp-report exports apple_pmp_report_set_request
 *    (a pmp-report that carries the locked update) the request goes through it: one read-modify-write under the report's lock, the same
 *    lock every other client of the word takes, with the acknowledge wait in the same call. Without that export (or before a report is
 *    bound) bit 10 is read-modify-written here: two MMIO accesses without that lock, so a concurrent write of another bit in that
 *    window can be lost.
 *    A failure at any step (PS-ACK missing) unwinds in the macOS order and leaves level 0; after three
 *    PS-ACK timeouts the driver stops trying until it is reloaded.
 */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_address.h>

#include "ane.h"
#include "ane_dvfs_req.h"

/* Exported by pmp-report when it carries the locked update; resolved with symbol_get() so this module neither requires it nor loads without it. */
int apple_pmp_report_set_request(const struct device_node *report_np, unsigned int id, bool enable, bool no_ack);

#define DVFS_ON_OFF		0x2000
#define DVFS_CMD_OFF		0xa00
#define DVFS_CMD_BASE		0x80000000U

#define PTD_UPDATE_OFF		0x10000
#define PTD_REG_PMP_STATUS	0x001
#define PTD_REG_PS_REQ		0x0f8
#define PTD_REG_PS_ACK		0x100
#define PTD_REG_ANE_LEVEL	0x10c
#define PTD_PMP_RUNNING		0x1
#define ANE_DVFS_MAX_TIMEOUTS	3
#define PTD_LEVEL_WORD		0x2000000000000000ULL

#define ANE_DVFS_LEVEL_MAX	5
#define ANE_DVFS_LEVEL_DEFAULT	5
#define ANE_DVFS_WINDOW_SIZE	0x4000
#define ANE_PTD_WINDOW_SIZE	0x20000
#define ANE_DVFS_STEP_US	1000

static const char *const dvfs_level_state[ANE_DVFS_LEVEL_MAX + 1] = {
	"level-0", "level-1", "level-2", "level-3", "level-4", "level-5",
};

static u64 ptd_read(struct ane_device *ane, u32 reg)
{
	return readq(ane->dvfs.ptd + (u64)reg * 16);
}

/*
 * The level asked for at each power-up: 5 by default (the 30 minute soak of 2026-10-30 ran 9,000 Parakeet encoder calls at level 5,
 * bit-exact, 199.0 ms per call, 49 C at the end), 4 to 1 or 0 to opt down. 0 asks for nothing and leaves the boot level.
 * Values above ANE_DVFS_LEVEL_MAX are clamped.
 */
static unsigned int dvfs_level = ANE_DVFS_LEVEL_DEFAULT;
module_param(dvfs_level, uint, 0644);
MODULE_PARM_DESC(dvfs_level, "ANE clock level asked for at power-up on the 16-inch M1 Max: 0 (the boot level, no request), 1 to 5 (default 5); read at each power-up");

static void ptd_write(struct ane_device *ane, u32 reg, u64 val)
{
	writeq(val, ane->dvfs.ptd + PTD_UPDATE_OFF + (u64)reg * 8);
}

static void dvfs_step(void)
{
	usleep_range(ANE_DVFS_STEP_US, ANE_DVFS_STEP_US * 2);
}

static void dvfs_cmd(struct ane_device *ane, u32 prev, u32 new)
{
	writel(DVFS_CMD_BASE | prev << 4 | new, ane->dvfs.dvfs + DVFS_CMD_OFF);
}

/*
 * Ask for or withdraw this driver's bit of PS-REQ. When pmp-report exports apple_pmp_report_set_request and a report is bound, the request goes through it:
 * one read-modify-write under the report's lock, shared with every other client of the word, and the acknowledge wait in the same call. Otherwise (the
 * export is missing, or no report is bound yet: -ENODEV) the masked read-modify-write of bit 10 here, without that lock, and the acknowledge poll here.
 */
static int ps_req_request(struct ane_device *ane, bool enable, bool no_ack)
{
	struct ane_dvfs *d = &ane->dvfs;
	u64 req, ack;
	int ret;

	if (d->set_request) {
		ret = d->set_request(d->report_np, PS_ANE_SYS_ID, enable, no_ack);
		if (ret != -ENODEV)
			return ret;
	}
	req = ptd_read(ane, PTD_REG_PS_REQ);
	ptd_write(ane, PTD_REG_PS_REQ, enable ? ane_dvfs_req_up(req) : ane_dvfs_req_down(req));
	if (no_ack)
		return 0;
	return read_poll_timeout(ptd_read, ack, !!(ack & PS_ANE_SYS) == enable, 200, 50000, false, ane, PTD_REG_PS_ACK);
}

/* The release half, in the macOS order. Safe to call from any point of the up sequence. */
static void ane_dvfs_release(struct ane_device *ane, bool level_set, bool psreq_set)
{
	struct ane_dvfs *d = &ane->dvfs;

	if (level_set) {
		dvfs_cmd(ane, d->level, 0);
		dvfs_step();
		ptd_write(ane, PTD_REG_ANE_LEVEL, PTD_LEVEL_WORD);
		dvfs_step();
	}
	if (psreq_set) {
		ps_req_request(ane, false, true);
		dvfs_step();
	}
	dvfs_cmd(ane, 0, 0);
	dvfs_step();
	writel(0, d->dvfs + DVFS_ON_OFF);
}

/* Called at the end of runtime resume: the partition is up and the engine is enabled. */
void ane_dvfs_up(struct ane_device *ane)
{
	struct ane_dvfs *d = &ane->dvfs;
	u64 req, lvl;
	unsigned int level = min(READ_ONCE(dvfs_level), (unsigned int)ANE_DVFS_LEVEL_MAX);
	int err;

	if (!d->dvfs || !d->ptd || d->active)
		return;
	if (!level) {
		d->state = "level-0-by-parameter";
		return;
	}
	if (d->ack_timeouts >= ANE_DVFS_MAX_TIMEOUTS) {
		d->state = "disabled-after-ps-ack-timeouts";
		return;
	}

	if ((ptd_read(ane, PTD_REG_PMP_STATUS) & PTD_PMP_RUNNING) != PTD_PMP_RUNNING) {
		d->state = "pmp-not-running";
		return;
	}
	if (readl(d->dvfs + DVFS_ON_OFF)) {
		d->state = "dvfs-busy";
		return;
	}
	req = ptd_read(ane, PTD_REG_PS_REQ);
	if (!ane_dvfs_req_may_up(req)) {
		d->state = "ps-req-already-set";
		return;
	}
	lvl = ptd_read(ane, PTD_REG_ANE_LEVEL);
	if (lvl != 0 && (lvl < PTD_LEVEL_WORD || lvl > (PTD_LEVEL_WORD | 5))) {
		d->state = "level-word-unexpected";
		return;
	}

	writel(1, d->dvfs + DVFS_ON_OFF);
	dvfs_step();
	dvfs_cmd(ane, 0, 0);
	dvfs_step();
	/* Re-read right before the read-modify-write: bit 10 must still be clear; every other bit is another client's and is kept as read. */
	req = ptd_read(ane, PTD_REG_PS_REQ);
	if (!ane_dvfs_req_may_up(req)) {
		d->state = "ps-req-changed";
		ane_dvfs_release(ane, false, false);
		return;
	}
	err = ps_req_request(ane, true, false);
	if (err) {
		d->ack_timeouts++;
		d->state = "ps-ack-timeout";
		ane_dvfs_release(ane, false, true);
		return;
	}
	if (!ane_dvfs_req_is_up(ptd_read(ane, PTD_REG_PS_REQ))) {
		d->state = "ps-req-readback-unexpected";
		ane_dvfs_release(ane, false, true);
		return;
	}
	dvfs_cmd(ane, 0, level);
	dvfs_step();
	ptd_write(ane, PTD_REG_ANE_LEVEL, PTD_LEVEL_WORD | level);

	d->level = level;
	d->active = true;
	d->state = dvfs_level_state[level];
	dev_dbg(ane->dev, "ANE level %u requested (DVFS_CMD reads 0x%08x)\n", level,
		readl(d->dvfs + DVFS_CMD_OFF));
}

/* Called at the start of runtime suspend, after the busy veto and before the partition goes down. */
void ane_dvfs_down(struct ane_device *ane)
{
	struct ane_dvfs *d = &ane->dvfs;

	if (!d->active)
		return;
	ane_dvfs_release(ane, true, true);
	d->active = false;
	d->state = "level-0";
}

static ssize_t perf_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ane_device *ane = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", ane->dvfs.state);
}
static DEVICE_ATTR_RO(perf_state);

static void __iomem *ane_dvfs_map(struct ane_device *ane, const char *name, resource_size_t want)
{
	struct device_node *np = ane->dev->of_node;
	struct resource res;
	int idx = of_property_match_string(np, "reg-names", name);

	if (idx < 0 || of_address_to_resource(np, idx, &res) || resource_size(&res) != want)
		return NULL;
	return of_iomap(np, idx);
}

int ane_dvfs_init(struct ane_device *ane)
{
	struct ane_dvfs *d = &ane->dvfs;
	int err;

	d->state = "not-configured";
	d->dvfs = ane_dvfs_map(ane, "dvfs", ANE_DVFS_WINDOW_SIZE);
	d->ptd = ane_dvfs_map(ane, "ptd", ANE_PTD_WINDOW_SIZE);
	if (!d->dvfs || !d->ptd) {
		if (d->dvfs)
			iounmap(d->dvfs);
		if (d->ptd)
			iounmap(d->ptd);
		d->dvfs = d->ptd = NULL;
		return 0;
	}
	d->state = "level-0";
	err = device_create_file(ane->dev, &dev_attr_perf_state);
	if (err) {
		iounmap(d->dvfs);
		iounmap(d->ptd);
		d->dvfs = d->ptd = NULL;
		d->state = "not-configured";
		return err;
	}
	/* The locked writer is optional: kernels whose pmp-report does not export it keep the masked read-modify-write above. */
	d->set_request = symbol_get(apple_pmp_report_set_request);
	if (d->set_request) {
		d->report_np = of_find_compatible_node(NULL, NULL, "apple,t6000-pmp-v2-report");
		if (!d->report_np) {
			symbol_put(apple_pmp_report_set_request);
			d->set_request = NULL;
		}
	}
	return 0;
}

void ane_dvfs_exit(struct ane_device *ane)
{
	struct ane_dvfs *d = &ane->dvfs;

	if (!d->dvfs)
		return;
	device_remove_file(ane->dev, &dev_attr_perf_state);
	ane_dvfs_down(ane);
	if (d->set_request) {
		of_node_put((struct device_node *)d->report_np);
		symbol_put(apple_pmp_report_set_request);
		d->set_request = NULL;
		d->report_np = NULL;
	}
	iounmap(d->dvfs);
	iounmap(d->ptd);
	d->dvfs = d->ptd = NULL;
}
