// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * J700/T8140 H17 RUN-release probe.
 *
 * The first module in this tree that *writes* the ANE engine. It runs
 * steps 1-4 of the RTBuddy start design in
 * docs/j700-h17-rtbuddy-branch.md:
 *
 *   1. raise ane_sys + ane_sub0/1/2 through genpd, require ACTUAL=0xf
 *   2. clear SCRATCH0..7, SCRATCH6 = 0 (RTBuddy mode), pulse SCRATCH7 1->0
 *   3. read RVBAR, require latched and non-zero; never write it
 *   4. CPU_CONTROL = 0, then = 0x10
 *
 * Step 5 (RTKit HELLO over the mailbox) is NOT implemented here.
 *
 * Every step is written to /var/log/h17-run.log with O_SYNC + fsync
 * before the next step is attempted, because this access class has
 * frozen the machine with no kernel message. If the box dies, the log
 * says exactly which write was in flight.
 *
 * Out of an abundance of caution the probe halts the CPU
 * (CPU_CONTROL = 0) after the observation window, then runs the Apple
 * power-off sequence before releasing the domains.
 *
 * Usage:
 *   sudo insmod ane_h17_run_probe.ko optin=1 dry=1   # no writes, log only
 *   sudo insmod ane_h17_run_probe.ko optin=1         # run the sequence
 */
#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/uaccess.h>

#define H17_PMGR_PS_NODE "/soc/power-management@300700000/power-controller@290"
#define H17_ANE_NODE "/soc/ane@400000000"
#define H17_DART0_NODE "/soc/iommu@401800000"
#define H17_DART1_NODE "/soc/iommu@401820000"
#define H17_DART2_NODE "/soc/iommu@401840000"
#define H17_VOTE_NODE "omarchy,t8140-ane-cpu-vote"
#define H17_LOG_PATH "/var/log/h17-run.log"

#define H17_PS_ACTUAL GENMASK(7, 4)
#define H17_PS_AUTO BIT(28)
#define H17_MAX_SUB 3

#define H17_CPU_CONTROL 0x1600044
#define H17_CPU_STATUS 0x1600048
#define H17_RVBAR 0x1050000
#define H17_SCRATCH0 0x1880020
#define H17_SCRATCH6 0x1880038
#define H17_SCRATCH7 0x188003c
#define H17_CPU_RUN 0x10

#define H17_PWGATE_PA 0x300724000
#define H17_PWGATE_SIZE 0x4000
#define H17_PWGATE_OFF_A 0x268c
#define H17_PWGATE_OFF_B 0x26b0

#define H17_POLL_MS 10
#define H17_POLL_N 300

static const u32 h17_sub_off[H17_MAX_SUB] = { 0xc000, 0xc008, 0xc010 };

static bool optin;
module_param(optin, bool, 0444);
MODULE_PARM_DESC(optin, "Must be 1 on J700");

static bool dry;
module_param(dry, bool, 0444);
MODULE_PARM_DESC(dry, "1 = read and log only, perform no engine writes");

static struct file *logf;

static void h17_log(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	loff_t pos = 0;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n <= 0)
		return;
	pr_info("ane_h17_run %s", buf);
	if (IS_ERR_OR_NULL(logf))
		return;
	kernel_write(logf, buf, n, &pos);
	vfs_fsync(logf, 1);
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

static int h17_poweroff(struct regmap *map)
{
	void __iomem *pw;
	u32 val;

	regmap_read(map, 0xc000, &val);
	h17_log("teardown 0xc000 before=%#x\n", val);
	regmap_write(map, 0xc000, val & ~H17_PS_AUTO);

	pw = ioremap(H17_PWGATE_PA, H17_PWGATE_SIZE);
	if (!pw) {
		h17_log("teardown pwgate map failed\n");
		return -ENOMEM;
	}
	writel(3, pw + H17_PWGATE_OFF_A);
	writel(0, pw + H17_PWGATE_OFF_B);
	h17_log("teardown 0x268c=%#x 0x26b0=%#x\n",
		readl(pw + H17_PWGATE_OFF_A), readl(pw + H17_PWGATE_OFF_B));
	iounmap(pw);

	regmap_write(map, 0xc000, 0x300);
	regmap_read(map, 0xc000, &val);
	h17_log("teardown 0xc000 after=%#x\n", val);
	return 0;
}

static int ane_h17_run_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *pd[H17_MAX_SUB] = { };
	struct device_link *link[H17_MAX_SUB] = { };
	struct device_node *ps, *ane;
	struct regmap *map;
	void __iomem *eng;
	unsigned int i, nsub;
	u32 word, status, scratch7, last_status = 0, last_sc7 = 0;
	u64 rvbar;
	bool all_on = true;
	int count, ret = 0;

	h17_log("=== run at uptime %llus dry=%d ===\n", ktime_get_seconds(), dry);

	count = of_count_phandle_with_args(dev->of_node, "power-domains",
					   "#power-domain-cells");
	if (count < 0)
		return count;
	nsub = min_t(unsigned int, count, H17_MAX_SUB);
	if (nsub != H17_MAX_SUB) {
		h17_log("REFUSED reason=%u-domains\n", nsub);
		return -ENODEV;
	}

	ps = of_find_node_by_path(H17_PMGR_PS_NODE);
	if (!ps)
		return -ENODEV;
	map = syscon_node_to_regmap(ps->parent);
	of_node_put(ps);
	if (IS_ERR(map))
		return PTR_ERR(map);

	ane = of_find_node_by_path(H17_ANE_NODE);
	if (!ane)
		return -ENODEV;

	for (i = 0; i < nsub; i++) {
		pd[i] = dev_pm_domain_attach_by_id(dev, i);
		if (IS_ERR_OR_NULL(pd[i])) {
			ret = IS_ERR(pd[i]) ? PTR_ERR(pd[i]) : -ENODEV;
			pd[i] = NULL;
			goto out;
		}
		link[i] = device_link_add(dev, pd[i], DL_FLAG_STATELESS |
					  DL_FLAG_PM_RUNTIME | DL_FLAG_RPM_ACTIVE);
		if (!link[i]) {
			ret = -EINVAL;
			goto out;
		}
	}
	h17_log("step1 domains raised\n");

	for (i = 0; i < nsub; i++) {
		if (regmap_read(map, h17_sub_off[i], &word))
			continue;
		h17_log("  sub%u(%#x)=%#x target=%#x actual=%#x\n", i,
			h17_sub_off[i], word, word & GENMASK(3, 0),
			FIELD_GET(H17_PS_ACTUAL, word));
		if (FIELD_GET(H17_PS_ACTUAL, word) != 0xf)
			all_on = false;
	}
	if (!all_on) {
		h17_log("REFUSED reason=domains-not-on\n");
		ret = -EAGAIN;
		goto out;
	}

	eng = of_iomap(ane, 0);
	if (!eng) {
		h17_log("engine map failed\n");
		ret = -ENOMEM;
		goto out;
	}

	rvbar = readq(eng + H17_RVBAR);
	status = readl(eng + H17_CPU_STATUS);
	h17_log("step3 RVBAR=%#llx CPU_STATUS=%#x CPU_CONTROL=%#x\n",
		rvbar, status, readl(eng + H17_CPU_CONTROL));
	if (!(rvbar & 1) || !rvbar) {
		h17_log("REFUSED reason=rvbar-not-latched\n");
		ret = -EIO;
		goto out_unmap;
	}

	if (dry) {
		h17_log("dry=1, no engine writes; stopping here\n");
		goto out_unmap;
	}

	/* Step 2: scratch init, RTBuddy mode, pulse. */
	for (i = 0; i < 8; i++)
		writel(0, eng + H17_SCRATCH0 + 4 * i);
	h17_log("step2 scratch cleared, SCRATCH6=%#x\n",
		readl(eng + H17_SCRATCH6));

	writel(0, eng + H17_SCRATCH6);
	h17_log("step2 SCRATCH6 := 0 (rtbuddy), now=%#x\n",
		readl(eng + H17_SCRATCH6));

	writel(1, eng + H17_SCRATCH7);
	h17_log("step2 SCRATCH7 := 1, now=%#x\n", readl(eng + H17_SCRATCH7));
	writel(0, eng + H17_SCRATCH7);
	h17_log("step2 SCRATCH7 := 0, now=%#x\n", readl(eng + H17_SCRATCH7));

	/* Step 4: RUN release. */
	writel(0, eng + H17_CPU_CONTROL);
	h17_log("step4 CPU_CONTROL := 0, now=%#x\n",
		readl(eng + H17_CPU_CONTROL));
	writel(H17_CPU_RUN, eng + H17_CPU_CONTROL);
	h17_log("step4 CPU_CONTROL := %#x, now=%#x status=%#x\n",
		H17_CPU_RUN, readl(eng + H17_CPU_CONTROL),
		readl(eng + H17_CPU_STATUS));

	/* Step 5 observation: watch for a change, log every change. */
	for (i = 0; i < H17_POLL_N; i++) {
		status = readl(eng + H17_CPU_STATUS);
		scratch7 = readl(eng + H17_SCRATCH7);
		if (status != last_status || scratch7 != last_sc7) {
			h17_log("poll %u status=%#x scratch7=%#x control=%#x\n",
				i, status, scratch7,
				readl(eng + H17_CPU_CONTROL));
			last_status = status;
			last_sc7 = scratch7;
		}
		msleep(H17_POLL_MS);
	}
	h17_log("observation window done status=%#x scratch7=%#x\n",
		readl(eng + H17_CPU_STATUS), readl(eng + H17_SCRATCH7));

	/* Halt, then the Apple power-off sequence. */
	writel(0, eng + H17_CPU_CONTROL);
	h17_log("halt CPU_CONTROL := 0, now=%#x status=%#x\n",
		readl(eng + H17_CPU_CONTROL), readl(eng + H17_CPU_STATUS));
out_unmap:
	iounmap(eng);
out:
	if (!dry)
		h17_poweroff(map);
	for (i = nsub; i-- > 0; ) {
		if (link[i])
			device_link_del(link[i]);
		if (pd[i])
			dev_pm_domain_detach(pd[i], true);
	}
	h17_log("released\n");
	of_node_put(ane);
	return ret;
}

static const struct of_device_id ane_h17_run_of_match[] = {
	{ .compatible = H17_VOTE_NODE },
	{}
};
MODULE_DEVICE_TABLE(of, ane_h17_run_of_match);

static struct platform_driver ane_h17_run_driver = {
	.probe = ane_h17_run_probe,
	.driver = {
		.name = "ane_h17_run_probe",
		.of_match_table = ane_h17_run_of_match,
	},
};

static int __init ane_h17_run_init(void)
{
	struct device_node *ane, *probe;
	int ret = -EPERM;

	if (!optin || !of_machine_is_compatible("apple,j700"))
		goto refused;
	ane = of_find_node_by_path(H17_ANE_NODE);
	probe = of_find_compatible_node(NULL, NULL, H17_VOTE_NODE);
	if (!ane || !probe || of_device_is_available(ane) ||
	    !of_device_is_available(probe) ||
	    !of_device_is_compatible(ane, "apple,t8140-ane") ||
	    !ane_h17_darts_disabled()) {
		ret = -EINVAL;
		goto put_nodes;
	}
	ret = 0;
put_nodes:
	of_node_put(probe);
	of_node_put(ane);
	if (ret)
		goto refused;

	logf = filp_open(H17_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND | O_SYNC,
			 0644);
	if (IS_ERR(logf)) {
		pr_err("ane_h17_run cannot open %s: %ld\n",
		       H17_LOG_PATH, PTR_ERR(logf));
		logf = NULL;
	}
	return platform_driver_register(&ane_h17_run_driver);
refused:
	pr_err("ane_h17_run RESULT verdict=REFUSED reason=board-optin-or-dt error=%d\n",
	       ret);
	return ret;
}

static void __exit ane_h17_run_exit(void)
{
	platform_driver_unregister(&ane_h17_run_driver);
	if (!IS_ERR_OR_NULL(logf)) {
		vfs_fsync(logf, 1);
		filp_close(logf, NULL);
	}
}

module_init(ane_h17_run_init);
module_exit(ane_h17_run_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J700 H17 RTBuddy RUN-release probe with durable logging");
