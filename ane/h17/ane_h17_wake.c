// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * J700/T8140 H17 stage 4, containment v2.
 *
 * The register-map gate stays closed for engine access: it lifts only
 * after the containment rehearsal (contain_rehearse=1) proves the
 * recovery path on this hardware (see
 * receipts/2026-10-31-j700-h17-containment-correction/README.md for the
 * v1 defects and the requirements, and
 * receipts/2026-10-31-j700-h17-containment-v2/README.md for this
 * design).
 *
 * contain_rehearse=1 runs the rehearsal and touches no ANE register:
 * it verifies the SoC watchdog (apple,t8140-wdt WD1) is disarmed and
 * ticking, arms it, pins a containment timer with an absolute expiry to
 * a different online CPU, and waits for that timer to panic
 * ("REHEARSAL"). If the timer does not fire, the rehearsal disarms the
 * watchdog, records "rehearsal-no-fire" in the marker and fails with
 * the machine alive.
 *
 * When the gate lifts, the engine phase runs in a kthread bound to one
 * CPU while the containment timer stays pinned to a different online
 * CPU: a CPU stalled mid-MMIO cannot take timers, so the watch never
 * shares the stalled core. Every completed access restarts the
 * watchdog counter (WD1 CUR_TIME) and pushes the timer deadline;
 * RESET_EN is verified clear before arming and cleared and verified on
 * release. Every pending access is appended to marker_file (O_SYNC)
 * and a storage failure aborts the run before the next access.
 */
#include <linux/bitfield.h>
#include <linux/compiler.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/timer.h>

/* The register-map gate (deliberately no module-parameter override): it
 * closed again on 2026-10-31 after the v1 containment failed; it lifts
 * only on rehearsal evidence, per the correction receipt. The mutation
 * control flips it to 1 in a scratch copy to prove the harness rejects
 * the unqualified map. */
#ifndef H17_REGISTER_MAP_QUALIFIED
#define H17_REGISTER_MAP_QUALIFIED 0
#endif

#define H17_ANE_NODE "/soc/ane@400000000"
#define H17_PMGR_PS_NODE "/soc/power-management@300700000/power-controller@290"
#define H17_PROBE_COMPAT "omarchy,t8140-ane-ps-probe"
#define H17_DART0_NODE "/soc/iommu@401800000"
#define H17_DART1_NODE "/soc/iommu@401820000"
#define H17_DART2_NODE "/soc/iommu@401840000"

#define H17_PS_OFF 0x290
#define H17_PS_ACTUAL GENMASK(7, 4)

#define H17_ENGINE_BASE 0x400000000ull
#define H17_ENGINE_MIN_SIZE 0x1900000

/* T8140-sourced rows (hal-map receipt): the doorbell block, IPI word,
 * mailbox-setup writes and the SCRATCH page are composed or exported by
 * the AppleT8140ANEHAL kext of this machine's macOS build. */
#define H17_DOORBELL 0x18a4000
#define H17_DOORBELL_ACK 0x18a4008
#define H17_DOORBELL_STATE0 0x18a4010
#define H17_DOORBELL_STATE1 0x18a4018
#define H17_IPI_PENDING 0x18ac000
#define H17_MBOX_SETUP0 0x1960000
#define H17_MBOX_SETUP1 0x1974200
#define H17_MBOX_SETUP2 0x195c284
#define H17_MBOX_SETUP3 0x195c288
/* SCRATCH0 candidates: the h16g rows put SCRATCH0 at +0x20 of the page,
 * the h18g table at +0x24. The alignment is still a rehearsal target. */
#define H17_SCRATCH_PAGE 0x1880000
#define H17_SCRATCH0 (H17_SCRATCH_PAGE + 0x20)
#define H17_SCRATCH0_ALT (H17_SCRATCH_PAGE + 0x24)
#define H17_SCRATCH_N 9
/* Unsourced on T8140 (h16g-row / M6 family parity only): behind
 * read_wrapper / mbox_peek, default off. */
#define H17_CPU_CONTROL 0x1600044
#define H17_CPU_STATUS 0x1600048
#define H17_RVBAR 0x1050000
#define H17_MBOX 0x1608000
#define H17_MBOX_ALT 0x196c000
#define H17_MBOX_A2I_CTRL 0x110
#define H17_MBOX_I2A_CTRL 0x114
#define H17_MBOX_I2A_RECV0 0x830
#define H17_MBOX_I2A_RECV1 0x838

#define H17_MBOX_EMPTY BIT(17)
#define H17_MBOX_ENABLE BIT(0)
#define H17_WAKE_ACK 0x08042006u
#define H17_MBOX_MAXMSG 8

/* The SoC watchdog: WD1 of the apple,t8140-wdt block. Register offsets
 * and the RESET_EN bit follow the kernel's drivers/watchdog/apple_wdt.c
 * (aurora tree); BITE_TIME is kept at the value that driver programmed
 * at probe, so no clock-rate arithmetic happens here. */
#define H17_WDT_NODE_COMPAT "apple,t8140-wdt"
#define H17_WDT_WD1_CUR_TIME 0x10
#define H17_WDT_WD1_BITE_TIME 0x14
#define H17_WDT_WD1_CTRL 0x1c
#define H17_WDT_CTRL_RESET_EN BIT(2)

static bool optin;
module_param(optin, bool, 0444);
MODULE_PARM_DESC(optin, "Must be 1 on J700 after preflight and stage 3 pass");

static bool confirm_wake;
module_param(confirm_wake, bool, 0444);
MODULE_PARM_DESC(confirm_wake, "1 = run the HAL control path: mailbox-setup writes, doorbell ring, ack");

static unsigned int poll_ms = 10;
module_param(poll_ms, uint, 0444);
MODULE_PARM_DESC(poll_ms, "Poll interval in ms (default 10)");

static unsigned int wake_wait_ms = 500;
module_param(wake_wait_ms, uint, 0444);
MODULE_PARM_DESC(wake_wait_ms, "Bounded DOORBELL_ACK/SCRATCH7 poll window after the ring (default 500)");

static unsigned int mbox_wait_ms = 500;
module_param(mbox_wait_ms, uint, 0444);
MODULE_PARM_DESC(mbox_wait_ms, "Bounded mailbox peek window after the wake polls (default 500)");

static unsigned int stop_wait_ms = 500;
module_param(stop_wait_ms, uint, 0444);
MODULE_PARM_DESC(stop_wait_ms, "Bounded wait for the raised child state to read back on (default 500)");

static unsigned int mbox_off = H17_MBOX;
module_param(mbox_off, uint, 0444);
MODULE_PARM_DESC(mbox_off, "ASC mailbox base for mbox_peek, engine-relative (default 0x1608000, alt 0x196c000)");

static unsigned int scratch0_off = H17_SCRATCH0;
module_param(scratch0_off, uint, 0444);
MODULE_PARM_DESC(scratch0_off, "SCRATCH0 offset, 0x1880020 (default) or 0x1880024");

static unsigned int contain_ms = 1000;
module_param(contain_ms, uint, 0444);
MODULE_PARM_DESC(contain_ms, "Panic when one engine access outlives this many ms (default 1000, 0 = off)");

static char *marker_file = "/var/lib/ane-h17/stage4-marker";
module_param(marker_file, charp, 0444);
MODULE_PARM_DESC(marker_file, "O_SYNC pending-access record, appended before every engine access");

static unsigned int contain_rehearse;
module_param(contain_rehearse, uint, 0444);
MODULE_PARM_DESC(contain_rehearse, "0=off, 1=fire rehearsal (panics, reboots), 2=lifecycle rehearsal (no panic)");

static bool read_wrapper;
module_param(read_wrapper, bool, 0444);
MODULE_PARM_DESC(read_wrapper, "1 = also read the unsourced h16g/M6 CPU wrapper trio");

static bool mbox_peek;
module_param(mbox_peek, bool, 0444);
MODULE_PARM_DESC(mbox_peek, "1 = peek the unsourced ASC mailbox at mbox_off");

/* The macOS power-up path for ane-type 0x200/0x210 raises one more pmgr
 * power state, 0xf at offset 0xc008 of the ANE pmgr window, beyond
 * ANE_SYS@0x290 (the h16g branch raises 0xc018/0xc020, a t6021-shaped
 * branch 0x4008..0x4030), before it touches the ASC. On J700 the first
 * stage 4 snapshot with ANE_SYS alone hung the machine at the engine
 * read pass, so child_ps=1 (the default) raises 0xc008 — idempotently,
 * skipped when ACTUAL is already 0xf — before the first engine access.
 * child_ps=0 keeps the run write-free, engine reads may then hang. */
#define H17_PMGR_CHILD_OFF 0xc008

static bool child_ps = true;
module_param(child_ps, bool, 0444);
MODULE_PARM_DESC(child_ps, "Raise the 0xc008 pmgr state before engine access (default on)");

static const u32 h17_pmgr_dump_offs[] = {
	0x2e0, 0x4000, 0x4008, 0x4010, 0x4018, 0x4020, 0x4028, 0x4030,
	0xc000, 0xc008, 0xc010, 0xc018, 0xc020,
};

struct ane_h17_wake {
	struct device *dev;
	void __iomem *engine;
	struct regmap *pmgr;
	struct file *marker;
	loff_t marker_pos;
	int marker_err;
	void __iomem *wdt;
	u32 wdt_ctrl0;
	int watch_cpu;
	int timer_armed;
	int rc;
	struct completion done;
};

/* Containment state: globals because the containment timer reads them
 * from another CPU while the probe CPU may be stalled mid-access. */
static u32 h17_pending_off;
static u32 h17_pending_seq;
static struct timer_list h17_contain_timer;

/* Watchdog ownership by verification: the apple_wdt driver only sets
 * RESET_EN when a watcher opens the device, and systemd's runtime
 * watchdog is off on this host (RuntimeWatchdogUSec=0). */
static void __iomem *h17_wdt_map(void)
{
	struct device_node *np;
	void __iomem *regs;

	np = of_find_compatible_node(NULL, NULL, H17_WDT_NODE_COMPAT);
	if (!np)
		return NULL;
	regs = of_iomap(np, 0);
	of_node_put(np);
	return regs;
}

static int h17_wdt_verify_disarmed(void __iomem *regs, u32 *ctrl0)
{
	u32 cur0, cur1;

	*ctrl0 = readl(regs + H17_WDT_WD1_CTRL);
	if (*ctrl0 & H17_WDT_CTRL_RESET_EN)
		return -EBUSY;
	if (!readl(regs + H17_WDT_WD1_BITE_TIME))
		return -EIO;
	cur0 = readl(regs + H17_WDT_WD1_CUR_TIME);
	msleep(2);
	cur1 = readl(regs + H17_WDT_WD1_CUR_TIME);
	if (cur1 == cur0)
		return -EIO;
	return 0;
}

static void h17_wdt_pet(void __iomem *regs)
{
	writel_relaxed(0, regs + H17_WDT_WD1_CUR_TIME);
}

static int h17_wdt_arm(void __iomem *regs, u32 ctrl0)
{
	writel_relaxed(0, regs + H17_WDT_WD1_CUR_TIME);
	writel_relaxed(ctrl0 | H17_WDT_CTRL_RESET_EN, regs + H17_WDT_WD1_CTRL);
	if (readl(regs + H17_WDT_WD1_CTRL) & H17_WDT_CTRL_RESET_EN)
		return 0;
	return -EIO;
}

static int h17_wdt_disarm(void __iomem *regs, u32 ctrl0)
{
	writel_relaxed(ctrl0 & ~H17_WDT_CTRL_RESET_EN, regs + H17_WDT_WD1_CTRL);
	if (readl(regs + H17_WDT_WD1_CTRL) & H17_WDT_CTRL_RESET_EN)
		return -EIO;
	return 0;
}

static void h17_wdt_map_release(void __iomem *regs)
{
	if (regs)
		iounmap(regs);
}

static void h17_contain_panic(bool rehearsal, u32 seq, u32 off)
{
	if (rehearsal)
		panic("ane_h17_wake REHEARSAL contain_ms=%u\n", contain_ms);
	panic("ane_h17_wake HANG contain_ms=%u access=%u off=%#x\n",
	      contain_ms, seq, off);
}

static struct timer_list h17_rehearse_timer;

static void h17_rehearse_fn(struct timer_list *t)
{
	h17_contain_panic(true, 0, 0);
}

/*
 * The rehearsal, mode 1 (fire): verify the watchdog is disowned and
 * ticking, record the armed state in the marker, pin a containment
 * timer with an absolute expiry to a different online CPU and wait.
 * Success is the panic itself; the machine reboots. Reaching the end
 * alive means the watch did not fire: disarm, record and fail.
 *
 * Mode 2 (lifecycle): the same verify/arm/disarm discipline on the
 * watchdog and the containment timer, including a pet and a bounded
 * dwell well inside the deadline — but no panic and no reboot. This is
 * the rung that proves the lifecycle on silicon without cost.
 */
static int h17_rehearse_run(unsigned int mode)
{
	void __iomem *regs;
	struct file *marker = NULL;
	u32 ctrl0 = 0;
	loff_t pos = 0;
	char buf[80];
	int len, cpu, err = 0;
	unsigned int dwell;

	regs = h17_wdt_map();
	if (!regs) {
		pr_err("ane_h17_wake RESULT stage=4 verdict=FAIL reason=wdt-node\n");
		return -EOPNOTSUPP;
	}
	err = h17_wdt_verify_disarmed(regs, &ctrl0);
	if (err == -EBUSY) {
		pr_err("ane_h17_wake RESULT stage=4 verdict=FAIL reason=wdt-armed\n");
		goto out;
	}
	if (err) {
		pr_err("ane_h17_wake RESULT stage=4 verdict=FAIL reason=wdt-verify\n");
		goto out;
	}
	marker = filp_open(marker_file, O_APPEND | O_CREAT | O_WRONLY | O_SYNC,
			   0600);
	if (IS_ERR(marker)) {
		pr_err("ane_h17_wake RESULT stage=4 verdict=FAIL reason=marker-open\n");
		err = -EIO;
		marker = NULL;
		goto out;
	}
	len = snprintf(buf, sizeof(buf), "rehearsal mode=%u armed contain_ms=%u\n",
		       mode, contain_ms);
	if (kernel_write(marker, buf, len, &pos) != len) {
		pr_err("ane_h17_wake RESULT stage=4 verdict=FAIL reason=marker-write\n");
		err = -EIO;
		goto close;
	}
	cpu = smp_processor_id() + 1;
	if (cpu >= num_online_cpus())
		cpu = 0;
	timer_setup(&h17_rehearse_timer, h17_rehearse_fn, TIMER_PINNED);
	h17_rehearse_timer.expires = jiffies + msecs_to_jiffies(contain_ms + 500);
	add_timer_on(&h17_rehearse_timer, cpu);

	if (mode == 1) {
		pr_info("ane_h17_wake rehearsal fire armed cpu=%d contain_ms=%u\n",
			cpu, contain_ms);
		/* The timer panics before this sleep ends; only a broken
		 * watch returns here. */
		msleep(contain_ms + 1000);
		timer_delete_sync(&h17_rehearse_timer);
		err = h17_wdt_disarm(regs, ctrl0);
		if (err) {
			pr_err("ane_h17_wake RESULT stage=4 verdict=FAIL reason=wdt-disarm\n");
			goto close;
		}
		len = snprintf(buf, sizeof(buf), "rehearsal-no-fire\n");
		kernel_write(marker, buf, len, &pos);
		pr_err("ane_h17_wake RESULT stage=4 verdict=FAIL reason=rehearsal-no-fire\n");
		err = -EIO;
		goto close;
	}

	/* Mode 2: lifecycle only. Arm the watchdog, exercise the pet,
	 * dwell well inside the timer deadline, then disarm and verify
	 * both. No panic, no reboot. */
	err = h17_wdt_arm(regs, ctrl0);
	if (err) {
		pr_err("ane_h17_wake RESULT stage=4 verdict=FAIL reason=wdt-arm\n");
		goto close;
	}
	h17_wdt_pet(regs);
	pr_info("ane_h17_wake rehearsal lifecycle armed cpu=%d contain_ms=%u\n",
		cpu, contain_ms);
	dwell = contain_ms / 4;
	msleep(dwell);
	timer_delete_sync(&h17_rehearse_timer);
	err = h17_wdt_disarm(regs, ctrl0);
	if (err) {
		pr_err("ane_h17_wake RESULT stage=4 verdict=FAIL reason=wdt-disarm\n");
		goto close;
	}
	len = snprintf(buf, sizeof(buf), "rehearsal-lifecycle-ok\n");
	if (kernel_write(marker, buf, len, &pos) != len) {
		pr_err("ane_h17_wake RESULT stage=4 verdict=FAIL reason=marker-write\n");
		err = -EIO;
		goto close;
	}
	pr_info("ane_h17_wake RESULT stage=4 verdict=PASS reason=rehearsal-lifecycle\n");
	err = 0;
close:
	filp_close(marker, NULL);
out:
	h17_wdt_map_release(regs);
	return err;
}

static struct timer_list h17_contain_timer;

static void h17_contain_fn(struct timer_list *t)
{
	h17_contain_panic(false, READ_ONCE(h17_pending_seq),
			   READ_ONCE(h17_pending_off));
}

/* Called before every engine access: name the pending access in the
 * O_SYNC marker, push the containment deadline, restart the watchdog
 * counter. A storage failure is remembered and aborts the run before
 * the next access. */
static void h17_engine_pre(struct ane_h17_wake *w, u32 off)
{
	char buf[64];
	int len;

	h17_pending_off = off;
	h17_pending_seq++;
	if (w->marker && !w->marker_err) {
		len = snprintf(buf, sizeof(buf), "pending access=%u off=%#x\n",
			       h17_pending_seq, off);
		if (kernel_write(w->marker, buf, len, &w->marker_pos) != len)
			w->marker_err = -EIO;
	}
	if (contain_ms)
		mod_timer(&h17_contain_timer,
			  jiffies + msecs_to_jiffies(contain_ms));
	if (w->wdt)
		h17_wdt_pet(w->wdt);
}

/* Every MMIO read is logged before it happens: a hang names the word. */
static u32 h17_rd(struct ane_h17_wake *w, u32 off, const char *what)
{
	u32 v;

	h17_engine_pre(w, off);
	dev_info(w->dev, "ane_h17_wake read off=%#x %s\n", off, what);
	v = readl(w->engine + off);
	dev_info(w->dev, "ane_h17_wake value off=%#x %s=%#x\n", off, what, v);
	return v;
}

/* The HAL's doorbell words are 64-bit (ldr/str). */
static u64 h17_rdq(struct ane_h17_wake *w, u32 off, const char *what)
{
	u64 v;

	h17_engine_pre(w, off);
	dev_info(w->dev, "ane_h17_wake read off=%#x %s\n", off, what);
	v = readq(w->engine + off);
	dev_info(w->dev, "ane_h17_wake value off=%#x %s=%#llx\n", off, what, v);
	return v;
}

static void h17_wrq(struct ane_h17_wake *w, u32 off, u64 v, const char *what)
{
	h17_engine_pre(w, off);
	dev_info(w->dev, "ane_h17_wake write off=%#x val=%#llx %s\n", off, v, what);
	writeq(v, w->engine + off);
}

static bool ane_h17_darts_disabled(void)
{
	static const char *const paths[] = {
		H17_DART0_NODE, H17_DART1_NODE, H17_DART2_NODE,
	};
	struct device_node *node;
	unsigned int i;
	bool okay = true;

	for (i = 0; i < ARRAY_SIZE(paths); i++) {
		node = of_find_node_by_path(paths[i]);
		if (!node || of_device_is_available(node) ||
		    !of_device_is_compatible(node, "apple,t8140-dart"))
			okay = false;
		of_node_put(node);
	}
	return okay;
}

/*
 * The engine phase, bound to one CPU by the probe. The containment
 * timer is pinned elsewhere; every access pushes its expiry and
 * restarts the watchdog counter, so a hang is answered by the panic
 * (which names the access) and, behind it, the watchdog reset.
 */
static int h17_engine_run(struct ane_h17_wake *w)
{
	struct device *pdev = w->dev;
	u32 scratch7 = 0, s7a, s7b;
	u64 state, state1, ring = 0;
	unsigned int i, msgs = 0, polls;
	bool wake_seen = false, ring_acked = false;
	int ret = 0;
	u32 ctrl0 = 0;

	/* Watchdog first: refuse the run unless the block is disowned,
	 * ticking, and arms cleanly. */
	w->wdt = h17_wdt_map();
	if (!w->wdt) {
		dev_err(pdev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=wdt-node\n");
		return -EOPNOTSUPP;
	}
	ret = h17_wdt_verify_disarmed(w->wdt, &ctrl0);
	if (ret == -EBUSY) {
		dev_err(pdev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=wdt-armed\n");
		goto fail_nowdt;
	}
	if (ret) {
		dev_err(pdev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=wdt-verify\n");
		goto fail_nowdt;
	}
	ret = h17_wdt_arm(w->wdt, ctrl0);
	if (ret) {
		dev_err(pdev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=wdt-arm\n");
		goto fail_nowdt;
	}
	w->wdt_ctrl0 = ctrl0;

	/* Arm the containment watch with an absolute expiry, pinned to
	 * the CPU chosen by the probe (never the engine CPU). */
	if (contain_ms) {
		timer_setup(&h17_contain_timer, h17_contain_fn, TIMER_PINNED);
		h17_contain_timer.expires =
			jiffies + msecs_to_jiffies(contain_ms);
		add_timer_on(&h17_contain_timer, w->watch_cpu);
		w->timer_armed = 1;
		dev_info(pdev, "ane_h17_wake containment armed cpu=%d contain_ms=%u\n",
			 w->watch_cpu, contain_ms);
	}

	if (w->marker_err) {
		dev_err(pdev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=marker-write\n");
		goto fail;
	}

	/* Read-only pass over the T8140-sourced engine block. No write yet. */
	for (i = 0; i < H17_SCRATCH_N && !w->marker_err; i++)
		h17_rd(w, scratch0_off + 4 * i, "SCRATCH");
	if (w->marker_err)
		goto fail;
	h17_rd(w, H17_IPI_PENDING, "IPI_PENDING");
	h17_rdq(w, H17_DOORBELL_STATE0, "DOORBELL_STATE0");
	h17_rdq(w, H17_DOORBELL_STATE1, "DOORBELL_STATE1");
	if (w->marker_err)
		goto fail;
	if (read_wrapper) {
		h17_rd(w, H17_CPU_STATUS, "CPU_STATUS");
		h17_rd(w, H17_CPU_CONTROL, "CPU_CONTROL");
		h17_rd(w, H17_RVBAR, "RVBAR_LO");
		h17_rd(w, H17_RVBAR + 4, "RVBAR_HI");
	}
	if (mbox_peek) {
		h17_rd(w, mbox_off + H17_MBOX_A2I_CTRL, "A2I_CTRL");
		h17_rd(w, mbox_off + H17_MBOX_I2A_CTRL, "I2A_CTRL");
	}
	if (w->marker_err)
		goto fail;

	if (!confirm_wake) {
		dev_info(pdev, "ane_h17_wake RESULT stage=4 verdict=PASS reason=asc-snapshot\n");
		goto release;
	}

	state = h17_rdq(w, H17_DOORBELL_STATE0, "DOORBELL_STATE0") & 3;
	state1 = h17_rdq(w, H17_DOORBELL_STATE1, "DOORBELL_STATE1");
	ring = state ^ 3;
	if (ring & state1) {
		dev_err(pdev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=doorbell-conflict state=%llu state1=%llu\n",
			state, state1);
		ret = -EIO;
		goto release;
	}
	if (state != 3) {
		h17_wrq(w, H17_DOORBELL, ring, "DOORBELL");
		polls = (wake_wait_ms + poll_ms - 1) / poll_ms;
		for (i = 0; i < polls && !w->marker_err; i++) {
			msleep(poll_ms);
			if ((h17_rdq(w, H17_DOORBELL_ACK, "DOORBELL_ACK") & 3) == ring)
				break;
		}
		state1 = h17_rdq(w, H17_DOORBELL_STATE1, "DOORBELL_STATE1");
		if (ring & state1) {
			dev_err(pdev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=doorbell-unclear state1=%llu\n",
				state1);
			ret = -EIO;
			goto release;
		}
		h17_wrq(w, H17_DOORBELL_ACK, ring, "DOORBELL_ACK");
		ring_acked = true;
	} else {
		dev_info(pdev, "ane_h17_wake doorbell state already 3, no ring\n");
	}

	h17_wrq(w, H17_MBOX_SETUP0, 1, "MBOX_SETUP0");
	h17_wrq(w, H17_MBOX_SETUP1, 0, "MBOX_SETUP1");
	h17_wrq(w, H17_MBOX_SETUP1, 1, "MBOX_SETUP1");
	h17_wrq(w, H17_MBOX_SETUP2, 1, "MBOX_SETUP2");
	h17_wrq(w, H17_MBOX_SETUP3, 1, "MBOX_SETUP3");

	s7a = scratch0_off + 4 * 7;
	s7b = scratch0_off + 4 * 8;
	polls = (wake_wait_ms + poll_ms - 1) / poll_ms;
	for (i = 0; i < polls && !w->marker_err; i++) {
		u32 ack_b;

		msleep(poll_ms);
		scratch7 = h17_rd(w, s7a, "SCRATCH7_A");
		ack_b = h17_rd(w, s7b, "SCRATCH7_B");
		if (scratch7 == H17_WAKE_ACK || ack_b == H17_WAKE_ACK) {
			wake_seen = true;
			break;
		}
	}
	dev_info(pdev, "ane_h17_wake wake wait done wake_seen=%d scratch7=%#x ring=%llx acked=%d\n",
		 wake_seen, scratch7, ring, ring_acked);

	if (mbox_peek) {
		polls = (mbox_wait_ms + poll_ms - 1) / poll_ms;
		for (i = 0; i < polls && msgs < H17_MBOX_MAXMSG && !w->marker_err; i++) {
			u32 ctrl_i2a;

			msleep(poll_ms);
			ctrl_i2a = h17_rd(w, mbox_off + H17_MBOX_I2A_CTRL, "I2A_CTRL");
			if (ctrl_i2a & H17_MBOX_EMPTY)
				continue;
			if (!(ctrl_i2a & H17_MBOX_ENABLE)) {
				dev_info(pdev, "ane_h17_wake mailbox not enabled ctrl=%#x\n", ctrl_i2a);
				break;
			}
			h17_rd(w, mbox_off + H17_MBOX_I2A_RECV0, "I2A_RECV0");
			h17_rd(w, mbox_off + H17_MBOX_I2A_RECV1, "I2A_RECV1");
			msgs++;
		}
		dev_info(pdev, "ane_h17_wake mailbox peek done mbox_off=%#x msgs=%u\n",
			 mbox_off, msgs);
	}

	if (wake_seen)
		dev_info(pdev, "ane_h17_wake RESULT stage=4 verdict=PASS reason=wake-word msgs=%u\n", msgs);
	else if (ring_acked)
		dev_info(pdev, "ane_h17_wake RESULT stage=4 verdict=PASS reason=doorbell-ack\n");
	else
		dev_info(pdev, "ane_h17_wake RESULT stage=4 verdict=PASS reason=quiet-stop\n");
	goto release;
fail:
	ret = -EIO;
release:
	if (w->timer_armed) {
		timer_delete_sync(&h17_contain_timer);
		w->timer_armed = 0;
	}
	if (h17_wdt_disarm(w->wdt, w->wdt_ctrl0))
		dev_crit(pdev, "ane_h17_wake failed to disarm the SoC watchdog: a reboot is required\n");
	if (w->marker) {
		char buf[32];
		int len = snprintf(buf, sizeof(buf), "done access=%u\n",
				   h17_pending_seq);

		kernel_write(w->marker, buf, len, &w->marker_pos);
	}
fail_nowdt:
	h17_wdt_map_release(w->wdt);
	w->wdt = NULL;
	return ret;
}

/*
 * The engine phase runs here, bound to one CPU; the containment timer
 * is pinned to a different online CPU and carries an absolute expiry
 * before the first access. The watchdog is verified, armed, petted per
 * access and disarmed with verification.
 */
static int h17_engine_fn(void *data)
{
	struct ane_h17_wake *w = data;

	w->rc = h17_engine_run(w);
	complete(&w->done);
	return 0;
}

static int ane_h17_wake_probe(struct platform_device *pdev)
{
	struct device_node *ane, *ps;
	struct ane_h17_wake *w;
	struct resource res;
	struct task_struct *eng;
	u32 word;
	unsigned int i, polls;
	int probe_cpu, ret;

	if (!pdev->dev.pm_domain) {
		dev_err(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=REFUSED reason=no-genpd\n");
		return -ENODEV;
	}
	w = devm_kzalloc(&pdev->dev, sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;
	w->dev = &pdev->dev;

	ps = of_find_node_by_path(H17_PMGR_PS_NODE);
	if (!ps)
		return -ENODEV;
	w->pmgr = syscon_node_to_regmap(ps->parent);
	of_node_put(ps);
	if (IS_ERR(w->pmgr))
		return PTR_ERR(w->pmgr);

	ane = of_find_node_by_path(H17_ANE_NODE);
	if (!ane)
		return -ENODEV;
	ret = of_address_to_resource(ane, 0, &res);
	of_node_put(ane);
	if (ret)
		return ret;
	if (res.start != H17_ENGINE_BASE || resource_size(&res) < H17_ENGINE_MIN_SIZE) {
		dev_err(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=engine-window start=%#llx size=%#llx\n",
			(unsigned long long)res.start, (unsigned long long)resource_size(&res));
		return -EINVAL;
	}
	if (scratch0_off != H17_SCRATCH0 && scratch0_off != H17_SCRATCH0_ALT) {
		dev_err(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=scratch0-off val=%#x\n",
			scratch0_off);
		return -EINVAL;
	}
	w->engine = devm_ioremap_resource(&pdev->dev, &res);
	if (IS_ERR(w->engine))
		return PTR_ERR(w->engine);
	if (!poll_ms)
		poll_ms = 1;

	/* The containment watch must live on a different online CPU than
	 * the one the engine phase is bound to: a CPU stalled mid-MMIO
	 * cannot take timers. */
	probe_cpu = smp_processor_id();
	w->watch_cpu = cpumask_next(probe_cpu, cpu_online_mask);
	if (w->watch_cpu >= nr_cpu_ids)
		w->watch_cpu = cpumask_first(cpu_online_mask);
	if (w->watch_cpu == probe_cpu || !cpu_online(w->watch_cpu)) {
		dev_err(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=no-watch-cpu\n");
		return -EIO;
	}

	/* The marker is mandatory: the run is refused, not degraded, when
	 * the durable record is unavailable. */
	w->marker = filp_open(marker_file, O_APPEND | O_CREAT | O_WRONLY | O_SYNC,
			      0600);
	if (IS_ERR(w->marker)) {
		dev_err(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=marker-open\n");
		w->marker = NULL;
		return -EIO;
	}

	pm_runtime_enable(&pdev->dev);
	ret = pm_runtime_resume_and_get(&pdev->dev);
	if (ret) {
		dev_err(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=power-vote error=%d\n", ret);
		goto close_marker;
	}
	ret = regmap_read(w->pmgr, H17_PS_OFF, &word);
	if (ret) {
		dev_err(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=pmgr-read error=%d\n", ret);
		goto release;
	}
	dev_info(&pdev->dev, "ane_h17_wake pmgr off=%#x word=%#x target=%#x actual=%#lx\n",
		 H17_PS_OFF, word, word & 0xf, FIELD_GET(H17_PS_ACTUAL, word));
	if (FIELD_GET(H17_PS_ACTUAL, word) != 0xf) {
		dev_err(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=actual-not-on\n");
		ret = -EIO;
		goto release;
	}

	for (i = 0; i < ARRAY_SIZE(h17_pmgr_dump_offs); i++) {
		dev_info(&pdev->dev, "ane_h17_wake pmgr read off=%#x\n", h17_pmgr_dump_offs[i]);
		if (regmap_read(w->pmgr, h17_pmgr_dump_offs[i], &word))
			dev_info(&pdev->dev, "ane_h17_wake pmgr off=%#x read-failed\n",
				 h17_pmgr_dump_offs[i]);
		else
			dev_info(&pdev->dev, "ane_h17_wake pmgr off=%#x word=%#x\n",
				 h17_pmgr_dump_offs[i], word);
	}

	/*
	 * Raise the 0xc008 child state before the first engine access:
	 * macOS does this before it touches the ASC, and the first
	 * snapshot attempt with ANE_SYS alone hung the machine at the
	 * engine read pass. Idempotent: skipped when ACTUAL is already 0xf.
	 */
	if (child_ps) {
		dev_info(&pdev->dev, "ane_h17_wake pmgr read off=%#x child-state\n",
			 H17_PMGR_CHILD_OFF);
		if (regmap_read(w->pmgr, H17_PMGR_CHILD_OFF, &word))
			word = 0;
		if (FIELD_GET(H17_PS_ACTUAL, word) != 0xf) {
			dev_info(&pdev->dev, "ane_h17_wake pmgr write off=%#x val=%#x child\n",
				 H17_PMGR_CHILD_OFF, 0xfu);
			regmap_write(w->pmgr, H17_PMGR_CHILD_OFF, 0xf);
			polls = (stop_wait_ms + poll_ms - 1) / poll_ms;
			for (i = 0; i < polls; i++) {
				msleep(poll_ms);
				if (regmap_read(w->pmgr, H17_PMGR_CHILD_OFF, &word))
					break;
				if (FIELD_GET(H17_PS_ACTUAL, word) == 0xf)
					break;
			}
		}
		dev_info(&pdev->dev, "ane_h17_wake pmgr off=%#x word=%#x child\n",
			 H17_PMGR_CHILD_OFF, word);
		if (FIELD_GET(H17_PS_ACTUAL, word) != 0xf) {
			dev_err(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=child-not-on\n");
			ret = -EIO;
			goto release;
		}
	}

	/*
	 * The engine phase runs bound to probe_cpu; the containment
	 * timer is pinned to watch_cpu with an absolute expiry set
	 * before the first access.
	 */
	init_completion(&w->done);
	eng = kthread_create_on_cpu(h17_engine_fn, w, probe_cpu,
				    "ane_h17_wake");
	if (IS_ERR(eng)) {
		dev_err(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=kthread error=%ld\n",
			PTR_ERR(eng));
		ret = PTR_ERR(eng);
		goto release;
	}
	wake_up_process(eng);
	wait_for_completion(&w->done);
	ret = w->rc;

release:
	if (w->marker) {
		filp_close(w->marker, NULL);
		w->marker = NULL;
	}
	pm_runtime_put_sync(&pdev->dev);
close_marker:
	pm_runtime_disable(&pdev->dev);
	return ret;
}

static const struct of_device_id ane_h17_wake_of_match[] = {
	{ .compatible = H17_PROBE_COMPAT },
	{}
};
MODULE_DEVICE_TABLE(of, ane_h17_wake_of_match);

static struct platform_driver ane_h17_wake_driver = {
	.probe = ane_h17_wake_probe,
	.driver = {
		.name = "ane_h17_wake",
		.of_match_table = ane_h17_wake_of_match,
	},
};

static int __init ane_h17_wake_init(void)
{
	if (!optin || !of_machine_is_compatible("apple,j700")) {
		pr_err("ane_h17_wake RESULT stage=4 verdict=REFUSED reason=board-optin-or-dt\n");
		return -EPERM;
	}
	if (contain_rehearse) {
		if (contain_rehearse != 1 && contain_rehearse != 2) {
			pr_err("ane_h17_wake RESULT stage=4 verdict=FAIL reason=rehearsal-mode val=%u\n",
			       contain_rehearse);
			return -EINVAL;
		}
		pr_info("ane_h17_wake rehearsal mode=%u: containment only, no ANE access\n",
			contain_rehearse);
		return h17_rehearse_run(contain_rehearse);
	}
	if (!H17_REGISTER_MAP_QUALIFIED) {
		pr_err("ane_h17_wake RESULT stage=4 verdict=REFUSED reason=unqualified-register-map; no power or MMIO access\n");
		return -EOPNOTSUPP;
	}
	return platform_driver_register(&ane_h17_wake_driver);
}

static void __exit ane_h17_wake_exit(void)
{
	platform_driver_unregister(&ane_h17_wake_driver);
}

module_init(ane_h17_wake_init);
module_exit(ane_h17_wake_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J700 H17 stage 4, containment v2: gate closed pending rehearsal");
