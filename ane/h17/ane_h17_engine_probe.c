// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * J700/T8140 H17 stage 4: first ANE engine-window read, inside the
 * stage-3c genpd window.
 *
 * Order:
 *   1. attach the three sub-domain genpds and add runtime-active links,
 *      exactly as stage 3c, so the pmgr driver raises them;
 *   2. read the PMGR words back and refuse if any ACTUAL != 0xf;
 *   3. only then map the ANE engine window (reg[0], 0x400000000) and read
 *      two whitelisted registers: CPU_STATUS 0x1600048 and RVBAR
 *      0x1050000 (64-bit). Both are on the H16/H17 read whitelist;
 *      CPU_STATUS reads 0x2a before firmware start.
 *
 * This is the first engine-window access and carries the residual hang
 * risk. If the domain set is insufficient for block access, the read can
 * stall the bus; recovery is a reboot. No other engine register, no
 * write, no firmware, no DART, no mailbox.
 */
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/device.h>
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

#define H17_PMGR_PS_NODE "/soc/power-management@300700000/power-controller@290"
#define H17_ANE_NODE "/soc/ane@400000000"
#define H17_DART0_NODE "/soc/iommu@401800000"
#define H17_DART1_NODE "/soc/iommu@401820000"
#define H17_DART2_NODE "/soc/iommu@401840000"
#define H17_VOTE_NODE "omarchy,t8140-ane-cpu-vote"
#define H17_PS_OFF 0x290
#define H17_PS_TARGET GENMASK(3, 0)
#define H17_PS_ACTUAL GENMASK(7, 4)
#define H17_MAX_SUB 3

/* H17 engine-window registers (pinned from the retained kernelcache;
 * see docs/j700-h17-linux-support-plan.md). */
#define H17_CPU_STATUS 0x1600048
#define H17_RVBAR 0x1050000

static const u32 h17_sub_off[H17_MAX_SUB] = { 0xc000, 0xc008, 0xc010 };
static const char *const h17_sub_name[H17_MAX_SUB] = { "sub0", "sub1", "sub2" };

static bool optin;
module_param(optin, bool, 0444);
MODULE_PARM_DESC(optin, "Must be 1 on J700 after stage 3c passed");

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

static int ane_h17_engine_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *pd[H17_MAX_SUB] = { };
	struct device_link *link[H17_MAX_SUB] = { };
	struct device_node *ps, *ane;
	struct regmap *map;
	void __iomem *eng;
	unsigned int i, nsub;
	u64 rvbar;
	u32 word, status;
	bool all_on = true;
	int count, ret = 0;

	count = of_count_phandle_with_args(dev->of_node, "power-domains",
					   "#power-domain-cells");
	if (count < 0)
		return count;
	nsub = min_t(unsigned int, count, H17_MAX_SUB);
	if (nsub != H17_MAX_SUB) {
		dev_err(dev, "ane_h17_engine RESULT stage=4 verdict=REFUSED reason=%u-domains\n", nsub);
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
			dev_err(dev, "ane_h17_engine RESULT stage=4 verdict=FAIL reason=attach-%u error=%d\n",
				i, ret);
			goto out;
		}
		link[i] = device_link_add(dev, pd[i], DL_FLAG_STATELESS |
					  DL_FLAG_PM_RUNTIME | DL_FLAG_RPM_ACTIVE);
		if (!link[i]) {
			ret = -EINVAL;
			dev_err(dev, "ane_h17_engine RESULT stage=4 verdict=FAIL reason=link-%u\n", i);
			goto out;
		}
	}

	/* Gate: refuse the engine read unless every domain is really on. */
	for (i = 0; i < nsub; i++) {
		ret = regmap_read(map, h17_sub_off[i], &word);
		if (ret) {
			dev_err(dev, "ane_h17_engine RESULT stage=4 verdict=FAIL reason=pmgr-read-%s error=%d\n",
				h17_sub_name[i], ret);
			goto out;
		}
		if (FIELD_GET(H17_PS_ACTUAL, word) != 0xf)
			all_on = false;
	}
	ret = regmap_read(map, H17_PS_OFF, &word);
	if (ret || FIELD_GET(H17_PS_ACTUAL, word) != 0xf)
		all_on = false;
	if (!all_on) {
		dev_err(dev, "ane_h17_engine RESULT stage=4 verdict=REFUSED reason=domains-not-on\n");
		ret = -EAGAIN;
		goto out;
	}

	eng = of_iomap(ane, 0);
	if (!eng) {
		dev_err(dev, "ane_h17_engine RESULT stage=4 verdict=FAIL reason=engine-iomap\n");
		ret = -ENOMEM;
		goto out;
	}

	status = readl(eng + H17_CPU_STATUS);
	rvbar = readq(eng + H17_RVBAR);
	dev_info(dev, "ane_h17_engine CPU_STATUS(%#x)=%#x RVBAR(%#x)=%#llx bit0=%u\n",
		 H17_CPU_STATUS, status, H17_RVBAR, rvbar, (u32)(rvbar & 1));
	dev_info(dev, "ane_h17_engine RESULT stage=4 verdict=PASS reason=engine-read-ok\n");
	ret = 0;
	iounmap(eng);
out:
	for (i = nsub; i-- > 0; ) {
		if (link[i])
			device_link_del(link[i]);
		if (pd[i])
			dev_pm_domain_detach(pd[i], true);
	}
	of_node_put(ane);
	return ret;
}

static const struct of_device_id ane_h17_engine_of_match[] = {
	{ .compatible = H17_VOTE_NODE },
	{}
};
MODULE_DEVICE_TABLE(of, ane_h17_engine_of_match);

static struct platform_driver ane_h17_engine_driver = {
	.probe = ane_h17_engine_probe,
	.driver = {
		.name = "ane_h17_engine_probe",
		.of_match_table = ane_h17_engine_of_match,
	},
};

static int __init ane_h17_engine_init(void)
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
	return platform_driver_register(&ane_h17_engine_driver);
refused:
	pr_err("ane_h17_engine RESULT stage=4 verdict=REFUSED reason=board-optin-or-dt error=%d\n", ret);
	return ret;
}

static void __exit ane_h17_engine_exit(void)
{
	platform_driver_unregister(&ane_h17_engine_driver);
}

module_init(ane_h17_engine_init);
module_exit(ane_h17_engine_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J700 H17 first ANE engine read; gated on genpd ACTUAL");
