// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * J700/T8140 H17 stage 3b: ANE_SYS vote plus PMGR-only reads of the
 * ANE sub-domain power-state words. No ANE or DART MMIO, reset, firmware
 * boot, mailbox, or task submit.
 *
 * Why this is safe: every access is a read of the always-on PMGR block,
 * the same class of access the pmgr-pwrstate driver performs for every
 * domain it manages. The engine window is never touched, so an unpowered
 * ANE cannot produce a stalled bus read. The only write is the genpd
 * vote on the existing ps_ane_sys provider (0x290), which stage 3
 * already qualified.
 *
 * Reuses the stage-3 overlay node (compatible omarchy,t8140-ane-ps-probe)
 * so no DT change or reboot is needed; load this module with the stage-3
 * module unloaded.
 *
 * PMGR field layout: aurora-linux b6f070c911934c9c377c4be939b883bf32f37acf,
 * drivers/pmdomain/apple/pmgr-pwrstate.c (TARGET 3:0, ACTUAL 7:4,
 * WAS_GATED 9:8, BUSY 11, AUTO_ENABLE 28).
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
#define H17_PS_TARGET GENMASK(3, 0)
#define H17_PS_WAS_GATED GENMASK(9, 8)
#define H17_PS_BUSY BIT(11)

/* ANE sub-domain ps words inside the PMGR window. The first (0xc000) is
 * the word the Apple driver writes for CPU power; the rest follow the
 * T8132/H16 chain and are read for evidence, not as gates. */
static const struct {
	const char *name;
	u32 off;
} h17_subdomains[] = {
	{ "sub0", 0xc000 },
	{ "sub1", 0xc008 },
	{ "sub2", 0xc010 },
	{ "sub3", 0xc018 },
};

static bool optin;
module_param(optin, bool, 0444);
MODULE_PARM_DESC(optin, "Must be 1 on J700 after stages 0-3 and preflight pass");

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

static void h17_print_word(struct device *dev, const char *name, u32 off, u32 word)
{
	dev_info(dev,
		 "ane_h17_cpu_ps %s off=%#x word=%#x target=%#lx actual=%#lx was_gated=%#lx busy=%u\n",
		 name, off, word, FIELD_GET(H17_PS_TARGET, word),
		 FIELD_GET(H17_PS_ACTUAL, word), FIELD_GET(H17_PS_WAS_GATED, word),
		 (word & H17_PS_BUSY) ? 1 : 0);
}

static int ane_h17_cpu_ps_probe(struct platform_device *pdev)
{
	struct device_node *ps;
	struct regmap *map;
	u32 word;
	unsigned int i;
	int ret;

	if (!pdev->dev.pm_domain) {
		dev_err(&pdev->dev, "ane_h17_cpu_ps RESULT stage=3b verdict=REFUSED reason=no-genpd\n");
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
		dev_err(&pdev->dev, "ane_h17_cpu_ps RESULT stage=3b verdict=FAIL reason=power-vote error=%d\n", ret);
		goto disable;
	}

	ret = regmap_read(map, H17_PS_OFF, &word);
	if (ret) {
		dev_err(&pdev->dev, "ane_h17_cpu_ps RESULT stage=3b verdict=FAIL reason=pmgr-read error=%d\n", ret);
		goto release;
	}
	h17_print_word(&pdev->dev, "ane_sys", H17_PS_OFF, word);
	if (FIELD_GET(H17_PS_ACTUAL, word) != 0xf) {
		dev_err(&pdev->dev, "ane_h17_cpu_ps RESULT stage=3b verdict=FAIL reason=actual-not-on\n");
		ret = -EIO;
		goto release;
	}

	for (i = 0; i < ARRAY_SIZE(h17_subdomains); i++) {
		ret = regmap_read(map, h17_subdomains[i].off, &word);
		if (ret) {
			dev_err(&pdev->dev, "ane_h17_cpu_ps RESULT stage=3b verdict=FAIL reason=pmgr-read-%s error=%d\n",
				h17_subdomains[i].name, ret);
			goto release;
		}
		h17_print_word(&pdev->dev, h17_subdomains[i].name,
			       h17_subdomains[i].off, word);
	}

	/* Informational only: the sub-domain words are evidence for where
	 * the CPU island sits, not a pass/fail gate yet. */
	dev_info(&pdev->dev, "ane_h17_cpu_ps RESULT stage=3b verdict=PASS reason=pmgr-read-ok\n");
	ret = 0;
release:
	pm_runtime_put_sync(&pdev->dev);
disable:
	pm_runtime_disable(&pdev->dev);
	return ret;
}

static const struct of_device_id ane_h17_cpu_ps_of_match[] = {
	{ .compatible = "omarchy,t8140-ane-ps-probe" },
	{}
};
MODULE_DEVICE_TABLE(of, ane_h17_cpu_ps_of_match);

static struct platform_driver ane_h17_cpu_ps_driver = {
	.probe = ane_h17_cpu_ps_probe,
	.driver = {
		.name = "ane_h17_cpu_ps_probe",
		.of_match_table = ane_h17_cpu_ps_of_match,
	},
};

static int __init ane_h17_cpu_ps_init(void)
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
	return platform_driver_register(&ane_h17_cpu_ps_driver);
refused:
	pr_err("ane_h17_cpu_ps RESULT stage=3b verdict=REFUSED reason=board-optin-or-dt error=%d\n", ret);
	return ret;
}

static void __exit ane_h17_cpu_ps_exit(void)
{
	platform_driver_unregister(&ane_h17_cpu_ps_driver);
}

module_init(ane_h17_cpu_ps_init);
module_exit(ane_h17_cpu_ps_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J700 H17 ANE sub-domain PMGR read; no ANE MMIO");
