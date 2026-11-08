// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * J700/T8140 H17 single-write probe.
 *
 * The scratch-clear loop in the RUN-release probe froze the machine, but
 * it wrote eight cells with no logging between them, so the log cannot
 * say which one. This probe writes cells individually and logs each
 * write before and after it, with fsync, so the last line in
 * /var/log/h17-write.log names the write that killed the box.
 *
 * Nothing else is attempted: no scratch clear loop, no CPU_CONTROL
 * release, no mailbox. It raises the domains, performs exactly the
 * selected writes, then runs the Apple power-off sequence and releases.
 *
 * Parameters:
 *   cells=<mask>   bit i set => write 0 to SCRATCH0 + 4*i   (default 0)
 *   cpu0=1         write 0 to CPU_CONTROL before the cells   (default 0)
 *
 * Examples:
 *   cells=0x01            only SCRATCH0
 *   cells=0x80            only SCRATCH7
 *   cpu0=1 cells=0x01     CPU_CONTROL then SCRATCH0
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
#define H17_LOG_PATH "/var/log/h17-write.log"

#define H17_PS_ACTUAL GENMASK(7, 4)
#define H17_PS_AUTO BIT(28)
#define H17_MAX_SUB 3

#define H17_CPU_CONTROL 0x1600044
#define H17_CPU_STATUS 0x1600048
#define H17_RVBAR 0x1050000
#define H17_SCRATCH0 0x1880020

#define H17_PWGATE_PA 0x300724000
#define H17_PWGATE_SIZE 0x4000
#define H17_PWGATE_OFF_A 0x268c
#define H17_PWGATE_OFF_B 0x26b0

static const u32 h17_sub_off[H17_MAX_SUB] = { 0xc000, 0xc008, 0xc010 };

static bool optin;
module_param(optin, bool, 0444);
MODULE_PARM_DESC(optin, "Must be 1 on J700");

static unsigned int cells;
module_param(cells, uint, 0444);
MODULE_PARM_DESC(cells, "bitmask: bit i writes 0 to SCRATCH0+4*i");

static bool cpu0;
module_param(cpu0, bool, 0444);
MODULE_PARM_DESC(cpu0, "write 0 to CPU_CONTROL before the scratch cells");

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
	pr_info("ane_h17_write %s", buf);
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

static void h17_poweroff(struct regmap *map)
{
	void __iomem *pw;
	u32 val;

	regmap_read(map, 0xc000, &val);
	regmap_write(map, 0xc000, val & ~H17_PS_AUTO);
	pw = ioremap(H17_PWGATE_PA, H17_PWGATE_SIZE);
	if (pw) {
		writel(3, pw + H17_PWGATE_OFF_A);
		writel(0, pw + H17_PWGATE_OFF_B);
		iounmap(pw);
	}
	regmap_write(map, 0xc000, 0x300);
	regmap_read(map, 0xc000, &val);
	h17_log("teardown 0xc000=%#x\n", val);
}

static int ane_h17_write_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *pd[H17_MAX_SUB] = { };
	struct device_link *link[H17_MAX_SUB] = { };
	struct device_node *ps, *ane;
	struct regmap *map;
	void __iomem *eng;
	unsigned int i, nsub;
	u32 word;
	bool all_on = true;
	int count, ret = 0;

	h17_log("=== run at uptime %llus cpu0=%d cells=%#x ===\n",
		ktime_get_seconds(), cpu0, cells);

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

	for (i = 0; i < nsub; i++) {
		if (regmap_read(map, h17_sub_off[i], &word))
			continue;
		if (FIELD_GET(H17_PS_ACTUAL, word) != 0xf)
			all_on = false;
	}
	h17_log("domains raised all_on=%d\n", all_on);
	if (!all_on) {
		ret = -EAGAIN;
		goto out;
	}

	eng = of_iomap(ane, 0);
	if (!eng) {
		ret = -ENOMEM;
		goto out;
	}

	h17_log("before RVBAR=%#llx CPU_STATUS=%#x CPU_CONTROL=%#x\n",
		readq(eng + H17_RVBAR), readl(eng + H17_CPU_STATUS),
		readl(eng + H17_CPU_CONTROL));

	if (cpu0) {
		h17_log("write BEGIN CPU_CONTROL(%#x) := 0\n", H17_CPU_CONTROL);
		writel(0, eng + H17_CPU_CONTROL);
		h17_log("write END   CPU_CONTROL now=%#x\n",
			readl(eng + H17_CPU_CONTROL));
	}

	for (i = 0; i < 8; i++) {
		u32 off = H17_SCRATCH0 + 4 * i;

		if (!(cells & BIT(i)))
			continue;
		h17_log("write BEGIN SCRATCH%u(%#x) := 0, was=%#x\n",
			i, off, readl(eng + off));
		writel(0, eng + off);
		h17_log("write END   SCRATCH%u now=%#x\n", i, readl(eng + off));
	}

	h17_log("writes complete CPU_STATUS=%#x\n", readl(eng + H17_CPU_STATUS));
	iounmap(eng);
out:
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

static const struct of_device_id ane_h17_write_of_match[] = {
	{ .compatible = H17_VOTE_NODE },
	{}
};
MODULE_DEVICE_TABLE(of, ane_h17_write_of_match);

static struct platform_driver ane_h17_write_driver = {
	.probe = ane_h17_write_probe,
	.driver = {
		.name = "ane_h17_write_probe",
		.of_match_table = ane_h17_write_of_match,
	},
};

static int __init ane_h17_write_init(void)
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
		pr_err("ane_h17_write cannot open %s: %ld\n",
		       H17_LOG_PATH, PTR_ERR(logf));
		logf = NULL;
	}
	return platform_driver_register(&ane_h17_write_driver);
refused:
	pr_err("ane_h17_write RESULT verdict=REFUSED reason=board-optin-or-dt error=%d\n",
	       ret);
	return ret;
}

static void __exit ane_h17_write_exit(void)
{
	platform_driver_unregister(&ane_h17_write_driver);
	if (!IS_ERR_OR_NULL(logf)) {
		vfs_fsync(logf, 1);
		filp_close(logf, NULL);
	}
}

module_init(ane_h17_write_init);
module_exit(ane_h17_write_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J700 H17 single-register write probe with durable log");
