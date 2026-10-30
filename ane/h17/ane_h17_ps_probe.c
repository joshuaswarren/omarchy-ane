// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * J700/T8140 H17 stage 3: one ANE_SYS power vote, one PMGR state read.
 * No ANE or DART MMIO, reset, firmware boot, mailbox, or task submit.
 * PMGR register semantics: aurora-linux
 * b6f070c911934c9c377c4be939b883bf32f37acf,
 * drivers/pmdomain/apple/pmgr-pwrstate.c (ACTUAL bits 7:4, ACTIVE 0xf).
 */
#include <linux/bitfield.h>
#include <linux/init.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

#define H17_PMGR_PS_NODE "/soc/power-management@300700000/power-controller@290"
#define H17_ANE_NODE "/soc/ane@400000000"
#define H17_DART0_NODE "/soc/iommu@401800000"
#define H17_DART1_NODE "/soc/iommu@401820000"
#define H17_DART2_NODE "/soc/iommu@401840000"
#define H17_PS_OFF 0x290
#define H17_PS_ACTUAL GENMASK(7, 4)

static bool optin;
module_param(optin, bool, 0444);
MODULE_PARM_DESC(optin, "Must be 1 on J700 after stages 0-2 and preflight pass");

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

static int ane_h17_ps_probe(struct platform_device *pdev)
{
	struct device_node *ps;
	struct regmap *map;
	u32 word;
	int ret;

	if (!pdev->dev.pm_domain) {
		dev_err(&pdev->dev, "ane_h17_ps RESULT stage=3 verdict=REFUSED reason=no-genpd\n");
		return -ENODEV;
	}
	ps = of_find_node_by_path(H17_PMGR_PS_NODE);
	if (!ps)
		return -ENODEV;
	map = syscon_node_to_regmap(ps->parent);
	of_node_put(ps);
	if (IS_ERR(map))
		return PTR_ERR(map);

	pm_runtime_enable(&pdev->dev);
	ret = pm_runtime_resume_and_get(&pdev->dev);
	if (ret) {
		dev_err(&pdev->dev, "ane_h17_ps RESULT stage=3 verdict=FAIL reason=power-vote error=%d\n", ret);
		goto disable;
	}
	ret = regmap_read(map, H17_PS_OFF, &word);
	if (ret) {
		dev_err(&pdev->dev, "ane_h17_ps RESULT stage=3 verdict=FAIL reason=pmgr-read error=%d\n", ret);
		goto release;
	}
	dev_info(&pdev->dev, "ane_h17_ps pmgr off=%#x word=%#x target=%#x actual=%#lx\n",
		 H17_PS_OFF, word, word & 0xf, FIELD_GET(H17_PS_ACTUAL, word));
	if (FIELD_GET(H17_PS_ACTUAL, word) != 0xf) {
		dev_err(&pdev->dev, "ane_h17_ps RESULT stage=3 verdict=FAIL reason=actual-not-on\n");
		ret = -EIO;
	} else {
		dev_info(&pdev->dev, "ane_h17_ps RESULT stage=3 verdict=PASS reason=ane-sys-on\n");
	}
release:
	pm_runtime_put_sync(&pdev->dev);
disable:
	pm_runtime_disable(&pdev->dev);
	return ret;
}

static const struct of_device_id ane_h17_ps_of_match[] = {
	{ .compatible = "omarchy,t8140-ane-ps-probe" },
	{}
};
MODULE_DEVICE_TABLE(of, ane_h17_ps_of_match);

static struct platform_driver ane_h17_ps_driver = {
	.probe = ane_h17_ps_probe,
	.driver = {
		.name = "ane_h17_ps_probe",
		.of_match_table = ane_h17_ps_of_match,
	},
};

static int __init ane_h17_ps_init(void)
{
	struct device_node *ane, *ps, *probe, *domain = NULL;
	u32 reg;
	int ret = -EPERM;

	if (!optin || !of_machine_is_compatible("apple,j700"))
		goto refused;
	ane = of_find_node_by_path(H17_ANE_NODE);
	ps = of_find_node_by_path(H17_PMGR_PS_NODE);
	probe = of_find_compatible_node(NULL, NULL, "omarchy,t8140-ane-ps-probe");
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
	return platform_driver_register(&ane_h17_ps_driver);
refused:
	pr_err("ane_h17_ps RESULT stage=3 verdict=REFUSED reason=board-optin-or-dt error=%d\n", ret);
	return ret;
}

static void __exit ane_h17_ps_exit(void)
{
	platform_driver_unregister(&ane_h17_ps_driver);
}

module_init(ane_h17_ps_init);
module_exit(ane_h17_ps_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J700 H17 ANE_SYS power-state probe; no ANE MMIO");
