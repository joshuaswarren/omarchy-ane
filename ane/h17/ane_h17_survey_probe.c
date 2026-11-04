// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * J700/T8140 H17 engine survey probe.
 *
 * Premise: ANE access can freeze this machine with no kernel log and no
 * panic. A probe that reports through dmesg therefore loses its own
 * evidence at the moment it matters. Every line here is written to a
 * file opened O_SYNC and fsync()ed, so whatever ran before a freeze is
 * on disk when the machine comes back.
 *
 * Read-only against the engine. Raises the sub-domains through genpd,
 * reads the engine words that have been measured on this board, and
 * releases the domains the same way the earlier probes did. It does NOT
 * run the Apple power-off sequence; use ane_h17_poweroff_probe.ko for
 * that.
 *
 * Usage:
 *   sudo insmod ane_h17_survey_probe.ko optin=1
 *   cat /var/log/h17-survey.log
 *
 * The log is append-only and timestamped per line, so consecutive runs
 * are separable.
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
#define H17_LOG_PATH "/var/log/h17-survey.log"

#define H17_PS_ACTUAL GENMASK(7, 4)
#define H17_PS_AUTO BIT(28)
#define H17_MAX_SUB 3

/* Engine words measured on this board (stage 4 and the teardown receipts). */
#define H17_CPU_CONTROL 0x1600044
#define H17_CPU_STATUS 0x1600048
#define H17_RVBAR 0x1050000
#define H17_SCRATCH0 0x1880020
#define H17_SCRATCH_N 8
#define H17_MBOX_A2I_CTRL 0x1608110
#define H17_MBOX_I2A_CTRL 0x1608114

/* PWGATE window, ADT resource 2. */
#define H17_PWGATE_PA 0x300724000
#define H17_PWGATE_SIZE 0x4000
#define H17_PWGATE_OFF_A 0x268c
#define H17_PWGATE_OFF_B 0x26b0

static const u32 h17_sub_off[H17_MAX_SUB] = { 0xc000, 0xc008, 0xc010 };

static bool optin;
module_param(optin, bool, 0444);
MODULE_PARM_DESC(optin, "Must be 1 on J700");

static struct file *h17_logf;

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
	pr_info("ane_h17_survey %s", buf);
	if (IS_ERR_OR_NULL(h17_logf))
		return;
	/* O_SYNC on open plus an explicit fsync: survive an abrupt reset. */
	kernel_write(h17_logf, buf, n, &pos);
	vfs_fsync(h17_logf, 1);
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

static void h17_survey_reads(struct regmap *map)
{
	void __iomem *eng;
	unsigned int i;
	u32 word;

	struct device_node *ane = of_find_node_by_path(H17_ANE_NODE);

	if (!ane) {
		h17_log("engine node missing\n");
		return;
	}
	eng = of_iomap(ane, 0);
	of_node_put(ane);
	if (!eng) {
		h17_log("engine map FAILED\n");
		return;
	}

	h17_log("CPU_CONTROL(%#x)=%#x\n", H17_CPU_CONTROL,
		readl(eng + H17_CPU_CONTROL));
	h17_log("CPU_STATUS(%#x)=%#x\n", H17_CPU_STATUS,
		readl(eng + H17_CPU_STATUS));
	h17_log("RVBAR(%#x)=%#llx\n", H17_RVBAR, readq(eng + H17_RVBAR));
	for (i = 0; i < H17_SCRATCH_N; i++) {
		word = readl(eng + H17_SCRATCH0 + 4 * i);
		h17_log("SCRATCH%u(%#x)=%#x\n", i,
			H17_SCRATCH0 + 4 * i, word);
	}
	h17_log("A2I_CTRL(%#x)=%#x I2A_CTRL(%#x)=%#x\n",
		H17_MBOX_A2I_CTRL, readl(eng + H17_MBOX_A2I_CTRL),
		H17_MBOX_I2A_CTRL, readl(eng + H17_MBOX_I2A_CTRL));

	iounmap(eng);
}

static void h17_survey_pmgr(struct regmap *map)
{
	unsigned int i;
	u32 word;

	for (i = 0; i < H17_MAX_SUB; i++) {
		if (regmap_read(map, h17_sub_off[i], &word))
			continue;
		h17_log("sub%u(%#x)=%#x target=%#x actual=%#x auto=%u\n", i,
			h17_sub_off[i], word, word & GENMASK(3, 0),
			FIELD_GET(H17_PS_ACTUAL, word),
			!!(word & H17_PS_AUTO));
	}
}

static int ane_h17_survey_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *pd[H17_MAX_SUB] = { };
	struct device_link *link[H17_MAX_SUB] = { };
	struct device_node *ps;
	struct regmap *map;
	unsigned int i, nsub;
	void __iomem *pw;
	int count, ret = 0;

	h17_log("probe start dev=%s\n", dev_name(dev));

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
		h17_log("domain %u raised\n", i);
	}

	h17_survey_pmgr(map);
	h17_survey_reads(map);

	pw = ioremap(H17_PWGATE_PA, H17_PWGATE_SIZE);
	if (pw) {
		h17_log("PWGATE 0x268c=%#x 0x26b0=%#x\n",
			readl(pw + H17_PWGATE_OFF_A),
			readl(pw + H17_PWGATE_OFF_B));
		iounmap(pw);
	}
	h17_log("survey complete\n");
out:
	for (i = nsub; i-- > 0; ) {
		if (link[i])
			device_link_del(link[i]);
		if (pd[i])
			dev_pm_domain_detach(pd[i], true);
	}
	h17_log("released\n");
	return ret;
}

static const struct of_device_id ane_h17_survey_of_match[] = {
	{ .compatible = H17_VOTE_NODE },
	{}
};
MODULE_DEVICE_TABLE(of, ane_h17_survey_of_match);

static struct platform_driver ane_h17_survey_driver = {
	.probe = ane_h17_survey_probe,
	.driver = {
		.name = "ane_h17_survey_probe",
		.of_match_table = ane_h17_survey_of_match,
	},
};

static int __init ane_h17_survey_init(void)
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

	h17_logf = filp_open(H17_LOG_PATH,
			     O_WRONLY | O_CREAT | O_APPEND | O_SYNC, 0644);
	if (IS_ERR(h17_logf)) {
		pr_err("ane_h17_survey cannot open %s: %ld\n",
		       H17_LOG_PATH, PTR_ERR(h17_logf));
		h17_logf = NULL;
	}
	h17_log("=== run at uptime %llus ===\n", ktime_get_seconds());
	return platform_driver_register(&ane_h17_survey_driver);
refused:
	pr_err("ane_h17_survey RESULT verdict=REFUSED reason=board-optin-or-dt error=%d\n",
	       ret);
	return ret;
}

static void __exit ane_h17_survey_exit(void)
{
	platform_driver_unregister(&ane_h17_survey_driver);
	if (!IS_ERR_OR_NULL(h17_logf)) {
		vfs_fsync(h17_logf, 1);
		filp_close(h17_logf, NULL);
	}
}

module_init(ane_h17_survey_init);
module_exit(ane_h17_survey_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J700 H17 engine survey with a disk-durable log");
