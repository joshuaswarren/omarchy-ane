// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * J700/T8140 H17 stage 4: bounded firmware wake and mailbox peek.
 *
 * One runtime-PM vote on the existing ps_ane_sys domain, one MMIO read
 * pass over the ASC wrapper, and — only with confirm_wake=1 — one RUN
 * write to CPU_CONTROL, a bounded poll of CPU_STATUS and SCRATCH7, a
 * read-only peek at the ASC mailbox, one RUN clear, and a bounded wait
 * for STOPPED. It never programs a DART, maps firmware, answers the
 * firmware, or submits a task.
 *
 * Register provenance (engine-relative offsets):
 * - CPU_CONTROL 0x1600044 / CPU_STATUS 0x1600048 / RVBAR 0x1050000 /
 *   SCRATCH0 0x1880020 / mailbox 0x1608000: the H16 rows of
 *   ane/h16/ane_h16_soc.c (the macOS 27.0 H16 interface kext owns the
 *   H17 ANE; see data/ane-soc/t8140.json "compatible":
 *   ane,t8132exclave), and ane/t6021/ane_t6021.h: "the 0x1600044 value
 *   belongs to the h16g/h17/h18g variant".
 * - The engine-relative SCRATCH block 0x1880000 on this SoC is
 *   corroborated by the AppleT8140ANEHAL kext of the mac17p kernelcache
 *   (macOS 26A428), which composes the physical address 0x401880000
 *   (engine 0x400000000 + 0x1880000); see the stage 4 receipt.
 * - CPU_CONTROL bit 4 RUN, CPU_STATUS bit 0 RUNNING / bit 1 STOPPED,
 *   the mailbox control and message offsets, and the wake word
 *   0x08042006 follow the H14/H16 modules (ane/t6021/ane_t6021.h,
 *   ane/h16/README-bringup.md). None of it is measured on T8140 yet;
 *   every read address is logged before the read so a hang names the
 *   word, and a hang costs one reboot.
 */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

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

#define H17_CPU_CONTROL 0x1600044
#define H17_CPU_STATUS 0x1600048
#define H17_RVBAR 0x1050000
#define H17_SCRATCH0 0x1880020
#define H17_SCRATCH_N 8
#define H17_MBOX 0x1608000
#define H17_MBOX_A2I_CTRL 0x110
#define H17_MBOX_I2A_CTRL 0x114
#define H17_MBOX_I2A_RECV0 0x830
#define H17_MBOX_I2A_RECV1 0x838

#define H17_CPU_RUN (1u << 4)
#define H17_ST_RUNNING (1u << 0)
#define H17_ST_STOPPED (1u << 1)
#define H17_MBOX_EMPTY BIT(17)
#define H17_MBOX_ENABLE BIT(0)
#define H17_WAKE_ACK 0x08042006u
#define H17_MBOX_MAXMSG 8

static bool optin;
module_param(optin, bool, 0444);
MODULE_PARM_DESC(optin, "Must be 1 on J700 after preflight and stage 3 pass");

static bool confirm_wake;
module_param(confirm_wake, bool, 0444);
MODULE_PARM_DESC(confirm_wake, "1 = write RUN to CPU_CONTROL (one write, then one clear)");

static unsigned int poll_ms = 10;
module_param(poll_ms, uint, 0444);
MODULE_PARM_DESC(poll_ms, "Poll interval in ms (default 10)");

static unsigned int wake_wait_ms = 500;
module_param(wake_wait_ms, uint, 0444);
MODULE_PARM_DESC(wake_wait_ms, "Bounded CPU_STATUS/SCRATCH7 poll window after RUN (default 500)");

static unsigned int mbox_wait_ms = 500;
module_param(mbox_wait_ms, uint, 0444);
MODULE_PARM_DESC(mbox_wait_ms, "Bounded mailbox peek window after the wake polls (default 500)");

static unsigned int stop_wait_ms = 500;
module_param(stop_wait_ms, uint, 0444);
MODULE_PARM_DESC(stop_wait_ms, "Bounded STOPPED wait after RUN clear (default 500)");

struct ane_h17_wake {
	struct device *dev;
	void __iomem *engine;
	struct regmap *pmgr;
};

/* Every MMIO read is logged before it happens: a hang names the word. */
static u32 h17_rd(struct ane_h17_wake *w, u32 off, const char *what)
{
	u32 v;

	dev_info(w->dev, "ane_h17_wake read off=%#x %s\n", off, what);
	v = readl(w->engine + off);
	dev_info(w->dev, "ane_h17_wake value off=%#x %s=%#x\n", off, what, v);
	return v;
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
	u32 word, status, ctrl, scratch7 = 0;
	unsigned int i, msgs = 0, polls;
	bool wake_seen = false;
	int ret;

	if (!pdev->dev.pm_domain) {
		dev_err(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=REFUSED reason=no-genpd\n");
		return -ENODEV;
	}
	w = devm_kzalloc(&pdev->dev, sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;
	w->dev = &pdev->dev;

	/* Same power vote as stage 3: the aurora PMGR driver owns the word. */
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
	w->engine = devm_ioremap_resource(&pdev->dev, &res);
	if (IS_ERR(w->engine))
		return PTR_ERR(w->engine);

	pm_runtime_enable(&pdev->dev);
	ret = pm_runtime_resume_and_get(&pdev->dev);
	if (ret) {
		dev_err(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=power-vote error=%d\n", ret);
		goto disable;
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

	/* Read-only pass over the ASC wrapper. No write yet. */
	status = h17_rd(w, H17_CPU_STATUS, "CPU_STATUS");
	ctrl = h17_rd(w, H17_CPU_CONTROL, "CPU_CONTROL");
	h17_rd(w, H17_RVBAR, "RVBAR_LO");
	h17_rd(w, H17_RVBAR + 4, "RVBAR_HI");
	for (i = 0; i < H17_SCRATCH_N; i++)
		h17_rd(w, H17_SCRATCH0 + 4 * i, "SCRATCH");
	h17_rd(w, H17_MBOX + H17_MBOX_A2I_CTRL, "A2I_CTRL");
	h17_rd(w, H17_MBOX + H17_MBOX_I2A_CTRL, "I2A_CTRL");

	if (!confirm_wake) {
		dev_info(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=PASS reason=asc-snapshot\n");
		goto release;
	}

	/* Gate the single RUN write on the stopped shape M1/M2/M6 showed. */
	if (status & H17_ST_RUNNING || !(status & H17_ST_STOPPED) ||
	    ctrl & H17_CPU_RUN) {
		dev_err(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=asc-not-stopped status=%#x ctrl=%#x\n",
			status, ctrl);
		ret = -EIO;
		goto release;
	}

	/* The one RUN write, then bounded polls. */
	dev_info(&pdev->dev, "ane_h17_wake write off=%#x val=%#x RUN\n", H17_CPU_CONTROL, H17_CPU_RUN);
	writel(H17_CPU_RUN, w->engine + H17_CPU_CONTROL);
	polls = (wake_wait_ms + poll_ms - 1) / poll_ms;
	for (i = 0; i < polls; i++) {
		msleep(poll_ms);
		status = h17_rd(w, H17_CPU_STATUS, "CPU_STATUS");
		scratch7 = h17_rd(w, H17_SCRATCH0 + 4 * 7, "SCRATCH7");
		if (scratch7 == H17_WAKE_ACK) {
			wake_seen = true;
			break;
		}
	}
	dev_info(&pdev->dev, "ane_h17_wake wake wait done wake_seen=%d status=%#x scratch7=%#x\n",
		 wake_seen, status, scratch7);

	/* Read-only mailbox peek. Reads pop the message; nothing is replied. */
	polls = (mbox_wait_ms + poll_ms - 1) / poll_ms;
	for (i = 0; i < polls && msgs < H17_MBOX_MAXMSG; i++) {
		u32 ctrl_i2a;

		msleep(poll_ms);
		ctrl_i2a = h17_rd(w, H17_MBOX + H17_MBOX_I2A_CTRL, "I2A_CTRL");
		if (ctrl_i2a & H17_MBOX_EMPTY)
			continue;
		if (!(ctrl_i2a & H17_MBOX_ENABLE)) {
			dev_info(&pdev->dev, "ane_h17_wake mailbox not enabled ctrl=%#x\n", ctrl_i2a);
			break;
		}
		h17_rd(w, H17_MBOX + H17_MBOX_I2A_RECV0, "I2A_RECV0");
		h17_rd(w, H17_MBOX + H17_MBOX_I2A_RECV1, "I2A_RECV1");
		msgs++;
	}
	dev_info(&pdev->dev, "ane_h17_wake mailbox peek done msgs=%u\n", msgs);

	/* The one RUN clear, then the bounded STOPPED wait. */
	dev_info(&pdev->dev, "ane_h17_wake write off=%#x val=%#x RUN-clear\n", H17_CPU_CONTROL, 0u);
	writel(0, w->engine + H17_CPU_CONTROL);
	polls = (stop_wait_ms + poll_ms - 1) / poll_ms;
	for (i = 0; i < polls; i++) {
		msleep(poll_ms);
		status = h17_rd(w, H17_CPU_STATUS, "CPU_STATUS");
		if (!(status & H17_ST_RUNNING) && (status & H17_ST_STOPPED))
			break;
	}
	if (status & H17_ST_RUNNING || !(status & H17_ST_STOPPED)) {
		/* Leave the power vote held: the domain stays on, as on the M6. */
		pm_runtime_disable(&pdev->dev);
		dev_crit(&pdev->dev, "ane_h17_wake core did not return to STOPPED (status=%#x): ANE_SYS stays on, a reboot is required, the vote is not released\n",
			 status);
		dev_err(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=FAIL reason=core-not-stopped status=%#x wake_seen=%d msgs=%u\n",
			status, wake_seen, msgs);
		return 0;
	}
	if (wake_seen && msgs)
		dev_info(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=PASS reason=wake-word-hello msgs=%u\n", msgs);
	else if (wake_seen)
		dev_info(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=PASS reason=wake-word\n");
	else if (msgs)
		dev_info(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=PASS reason=hello-msgs msgs=%u\n", msgs);
	else
		dev_info(&pdev->dev, "ane_h17_wake RESULT stage=4 verdict=PASS reason=quiet-stop\n");

release:
	pm_runtime_put_sync(&pdev->dev);
disable:
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

	if (!optin || !of_machine_is_compatible("apple,j700"))
		goto refused;
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
	if (ret)
		goto refused;
	return platform_driver_register(&ane_h17_wake_driver);
refused:
	pr_err("ane_h17_wake RESULT stage=4 verdict=REFUSED reason=board-optin-or-dt error=%d\n", ret);
	return ret;
}

static void __exit ane_h17_wake_exit(void)
{
	platform_driver_unregister(&ane_h17_wake_driver);
}

module_init(ane_h17_wake_init);
module_exit(ane_h17_wake_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J700 H17 bounded firmware wake and mailbox peek; no DART or task");
