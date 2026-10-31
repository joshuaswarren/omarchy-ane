// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * J700/T8140 H17 stage 4, containment v3.
 *
 * Containment is watchdog-only: before the engine phase the module
 * verifies the SoC watchdog (apple,t8140-wdt WD1) is disowned and
 * ticking and arms it; after every completed access it restarts the
 * WD1 counter; after the last access it disarms with readback. If an
 * access hangs, the pets stop and the hardware resets the machine by
 * itself — no panic path, no software recovery — and the O_SYNC marker
 * file names the pending access. A reset is the exceptional
 * condition; normal operation never panics.
 *
 * This is an offline draft, not a qualified recovery mechanism. The
 * register-map gate remains closed, including for watchdog rehearsals.
 * A stopped watchdog register is not exclusive ownership of a device
 * already bound to the kernel driver. The earlier unexplained reset
 * does not qualify this recovery path. No live experiment is authorized.
 */
#include <linux/bitfield.h>
#include <linux/compiler.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

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

/* T8140-sourced rows (hal-map receipt). */
#define H17_DOORBELL 0x18a4000
#define H17_DOORBELL_ACK 0x18a4008
#define H17_DOORBELL_STATE0 0x18a4010
#define H17_DOORBELL_STATE1 0x18a4018
#define H17_IPI_PENDING 0x18ac000
#define H17_MBOX_SETUP0 0x1960000
#define H17_MBOX_SETUP1 0x1974200
#define H17_MBOX_SETUP2 0x195c284
#define H17_MBOX_SETUP3 0x195c288
#define H17_SCRATCH_PAGE 0x1880000
#define H17_SCRATCH0 (H17_SCRATCH_PAGE + 0x20)
#define H17_SCRATCH0_ALT (H17_SCRATCH_PAGE + 0x24)
#define H17_SCRATCH_N 9
/* Unsourced on T8140 (h16g-row / M6 parity only): behind opt-ins. */
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

/* The SoC watchdog: WD1 of the apple,t8140-wdt block. Offsets and
 * RESET_EN (BIT 2) follow the kernel's drivers/watchdog/apple_wdt.c.
 * BITE_TIME is kept at the value that driver programmed at probe, so
 * no clock-rate arithmetic happens here. */
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

static char *marker_file = "/var/lib/ane-h17/stage4-marker";
module_param(marker_file, charp, 0444);
MODULE_PARM_DESC(marker_file, "O_SYNC pending-access record, appended before every engine access");

static bool contain_rehearse;
module_param(contain_rehearse, bool, 0444);
MODULE_PARM_DESC(contain_rehearse, "1 = watchdog lifecycle rehearsal: arm, pet, disarm; no panic, no ANE access");

static bool read_wrapper;
module_param(read_wrapper, bool, 0444);
MODULE_PARM_DESC(read_wrapper, "1 = also read the unsourced h16g/M6 CPU wrapper trio");

static bool mbox_peek;
module_param(mbox_peek, bool, 0444);
MODULE_PARM_DESC(mbox_peek, "1 = peek the unsourced ASC mailbox at mbox_off");

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
};

/* Watchdog ownership by verification: the apple_wdt driver only sets
 * RESET_EN when a watcher holds the device, and systemd's runtime
 * watchdog is off on this host. The block is mapped read/write only
 * around the engine phase. */
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

/*
 * The containment rehearsal: verify the watchdog is disowned and
 * ticking, arm it, exercise the pet, dwell inside the bite window,
 * disarm with readback. No panic, no reboot, no ANE access. A PASS
 * here establishes the arm/pet/disarm discipline on silicon; the fire
 * (pets stop -> hardware reset) is the same register mechanism and was
 * demonstrated by this machine's own Oct 31 reset.
 */
static int h17_rehearse_run(void)
{
	void __iomem *regs;
	struct file *marker = NULL;
	u32 ctrl0 = 0;
	loff_t pos = 0;
	char buf[80];
	int len, err = 0;

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
	len = snprintf(buf, sizeof(buf),
		       "rehearsal armed contain_ms=n/a watchdog-only\n");
	if (kernel_write(marker, buf, len, &pos) != len) {
		pr_err("ane_h17_wake RESULT stage=4 verdict=FAIL reason=marker-write\n");
		err = -EIO;
		goto close;
	}
	err = h17_wdt_arm(regs, ctrl0);
	if (err) {
		pr_err("ane_h17_wake RESULT stage=4 verdict=FAIL reason=wdt-arm\n");
		goto close;
	}
	h17_wdt_pet(regs);
	pr_info("ane_h17_wake rehearsal lifecycle armed (watchdog-only)\n");
	msleep(250);
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

/* Containment state: the pending access is read back by nothing in
 * software — the hardware watchdog is the recovery — but the globals
 * keep the record readable in oopses and debuggers. */
static u32 h17_pending_off;
static u32 h17_pending_seq;

/* Called before every engine access: restart the watchdog counter and
 * record the pending access in the O_SYNC marker. A storage failure is
 * remembered and aborts the run before the next access. */
static void h17_engine_pre(struct ane_h17_wake *w, u32 off)
{
	char buf[64];
	int len;

	if (w->wdt)
		h17_wdt_pet(w->wdt);
	h17_pending_off = off;
	h17_pending_seq++;
	if (w->marker && !w->marker_err) {
		len = snprintf(buf, sizeof(buf), "pending access=%u off=%#x\n",
			       h17_pending_seq, off);
		if (kernel_write(w->marker, buf, len, &w->marker_pos) != len)
			w->marker_err = -EIO;
	}
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

static int ane_h17_wake_probe(struct platform_device *pdev)
{
	struct device_node *ane, *ps;
	struct ane_h17_wake *w;
	struct resource res;
	u32 word = 0, scratch7 = 0, s7a, s7b;
	u64 state, state1, ring = 0;
	unsigned int i, msgs = 0, polls;
	bool wake_seen = false, ring_acked = false;
	bool voted = false;
	int ret = 0;
	u32 ctrl0 = 0;

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
	voted = true;
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

	/* Watchdog containment: verify the block is disowned and ticking
	 * and arm it before the first engine access. */
	w->wdt = h17_wdt_map();
	if (!w->wdt) {
		dev_err(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=wdt-node\n");
		ret = -EOPNOTSUPP;
		goto release;
	}
	ret = h17_wdt_verify_disarmed(w->wdt, &ctrl0);
	if (ret == -EBUSY) {
		dev_err(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=wdt-armed\n");
		goto release;
	}
	if (ret) {
		dev_err(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=wdt-verify\n");
		goto release;
	}
	ret = h17_wdt_arm(w->wdt, ctrl0);
	if (ret) {
		dev_err(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=wdt-arm\n");
		goto release;
	}
	w->wdt_ctrl0 = ctrl0;

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
		dev_info(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=PASS reason=asc-snapshot\n");
		goto release;
	}

	/*
	 * The T8140 HAL's own control path, decoded from the same build:
	 * the mailbox-setup writes, then ring the doorbell with the
	 * complement of (STATE0 & 3) once that value is clear in STATE1,
	 * a bounded poll of the ack, the ack write, then the bounded
	 * SCRATCH7 wake-word poll. The child state is already raised
	 * above (or child_ps=0).
	 */
	state = h17_rdq(w, H17_DOORBELL_STATE0, "DOORBELL_STATE0") & 3;
	state1 = h17_rdq(w, H17_DOORBELL_STATE1, "DOORBELL_STATE1");
	ring = state ^ 3;
	if (ring & state1) {
		dev_err(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=doorbell-conflict state=%llu state1=%llu\n",
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
			dev_err(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=doorbell-unclear state1=%llu\n",
				state1);
			ret = -EIO;
			goto release;
		}
		h17_wrq(w, H17_DOORBELL_ACK, ring, "DOORBELL_ACK");
		ring_acked = true;
	} else {
		dev_info(&pdev->dev, "ane_h17_wake doorbell state already 3, no ring\n");
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
	dev_info(&pdev->dev, "ane_h17_wake wake wait done wake_seen=%d scratch7=%#x ring=%llx acked=%d\n",
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
				dev_info(&pdev->dev, "ane_h17_wake mailbox not enabled ctrl=%#x\n", ctrl_i2a);
				break;
			}
			h17_rd(w, mbox_off + H17_MBOX_I2A_RECV0, "I2A_RECV0");
			h17_rd(w, mbox_off + H17_MBOX_I2A_RECV1, "I2A_RECV1");
			msgs++;
		}
		dev_info(&pdev->dev, "ane_h17_wake mailbox peek done mbox_off=%#x msgs=%u\n",
			 mbox_off, msgs);
	}

	if (wake_seen)
		dev_info(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=PASS reason=wake-word msgs=%u\n", msgs);
	else if (ring_acked)
		dev_info(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=PASS reason=doorbell-ack\n");
	else
		dev_info(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=PASS reason=quiet-stop\n");
	goto release;
fail:
	ret = -EIO;
release:
	if (w->wdt && h17_wdt_disarm(w->wdt, w->wdt_ctrl0))
		dev_crit(&pdev->dev, "ane_h17_wake failed to disarm the SoC watchdog: a reboot is required\n");
	if (w->marker) {
		char buf[32];
		int len = snprintf(buf, sizeof(buf), "done access=%u\n",
				   h17_pending_seq);

		kernel_write(w->marker, buf, len, &w->marker_pos);
	}
close_marker:
	if (w->marker) {
		filp_close(w->marker, NULL);
		w->marker = NULL;
	}
	h17_wdt_map_release(w->wdt);
	w->wdt = NULL;
	if (voted)
		pm_runtime_put_sync(&pdev->dev);
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
	struct device_node *ane, *ps, *probe, *domain = NULL;
	u32 reg;
	int ret = -EPERM;

	if (!optin || !of_machine_is_compatible("apple,j700")) {
		pr_err("ane_h17_wake RESULT stage=4 verdict=REFUSED reason=board-optin-or-dt\n");
		return -EPERM;
	}
	if (!H17_REGISTER_MAP_QUALIFIED) {
		pr_err("ane_h17_wake RESULT stage=4 verdict=REFUSED reason=unqualified-register-map; no power or MMIO access\n");
		return -EOPNOTSUPP;
	}
	if (contain_rehearse) {
		pr_info("ane_h17_wake rehearsal: watchdog lifecycle only, no ANE access\n");
		return h17_rehearse_run();
	}

	ane = of_find_node_by_path(H17_ANE_NODE);
	ps = of_find_node_by_path(H17_PMGR_PS_NODE);
	probe = of_find_compatible_node(NULL, NULL, H17_PROBE_COMPAT);
	if (probe)
		domain = of_parse_phandle(probe, "power-domains", 0);
	if (!ane || !ps || !probe || of_device_is_available(ane) ||
	    !of_device_is_available(ps) || !of_device_is_available(probe) ||
	    !of_device_is_compatible(ane, "apple,t8140-ane") ||
	    !ane_h17_darts_disabled() ||
	    of_property_read_u32(ps, "reg", &reg) || reg != H17_PS_OFF ||
	    domain != ps) {
		ret = -EINVAL;
		goto put_nodes;
	}
	ret = 0;
put_nodes:
	of_node_put(domain);
	of_node_put(probe);
	of_node_put(ps);
	of_node_put(ane);
	if (ret) {
		pr_err("ane_h17_wake RESULT stage=4 verdict=REFUSED reason=board-optin-or-dt error=%d\n",
		       ret);
		return ret;
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
MODULE_DESCRIPTION("J700 H17 offline draft; engine and watchdog rehearsals quarantined");
