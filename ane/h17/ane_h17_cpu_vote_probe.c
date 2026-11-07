// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * J700/T8140 H17 stage 3c: raise the ANE sub-domain genpds through the
 * pmgr driver and read their state back. No ANE or DART MMIO.
 *
 * The domains come from the stage-3c overlay (ps_ane_sub0/1/2 at PMGR
 * 0xc000/0xc008/0xc010). This module attaches to each, adds a
 * runtime-active device link so the pmgr driver performs its TARGET
 * write + ACTUAL poll, then reads the same PMGR words back. It never
 * writes a ps-word itself and never touches the engine window, so an
 * unpowered ANE cannot produce a stalled bus read.
 *
 * The engine remains untouched even if every domain reports ACTUAL=0xf;
 * that read is a separate, later gate.
 */
#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
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
#define H17_PS_WAS_GATED GENMASK(9, 8)
#define H17_PS_BUSY BIT(11)
#define H17_MAX_SUB 3

static const u32 h17_sub_off[H17_MAX_SUB] = { 0xc000, 0xc008, 0xc010 };
static const char *const h17_sub_name[H17_MAX_SUB] = { "sub0", "sub1", "sub2" };

static bool optin;
module_param(optin, bool, 0444);
MODULE_PARM_DESC(optin, "Must be 1 on J700 after stages 0-3b and preflight pass");

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
		 "ane_h17_cpu_vote %s off=%#x word=%#x target=%#lx actual=%#lx was_gated=%#lx busy=%u\n",
		 name, off, word, FIELD_GET(H17_PS_TARGET, word),
		 FIELD_GET(H17_PS_ACTUAL, word), FIELD_GET(H17_PS_WAS_GATED, word),
		 (word & H17_PS_BUSY) ? 1 : 0);
}

static int ane_h17_cpu_vote_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *pd[H17_MAX_SUB] = { };
	struct device_link *link[H17_MAX_SUB] = { };
	struct device_node *ps;
	struct regmap *map;
	unsigned int i, nsub;
	bool all_on = true;
	u32 word;
	int count, ret = 0;

	count = of_count_phandle_with_args(dev->of_node, "power-domains",
					   "#power-domain-cells");
	if (count < 0)
		return count;
	nsub = min_t(unsigned int, count, H17_MAX_SUB);
	if (!nsub) {
		dev_err(dev, "ane_h17_cpu_vote RESULT stage=3c verdict=REFUSED reason=no-power-domains\n");
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
			dev_err(dev, "ane_h17_cpu_vote RESULT stage=3c verdict=FAIL reason=attach-%u error=%d\n",
				i, ret);
			goto out;
		}
		link[i] = device_link_add(dev, pd[i], DL_FLAG_STATELESS |
					  DL_FLAG_PM_RUNTIME | DL_FLAG_RPM_ACTIVE);
		if (!link[i]) {
			ret = -EINVAL;
			dev_err(dev, "ane_h17_cpu_vote RESULT stage=3c verdict=FAIL reason=link-%u\n", i);
			goto out;
		}
	}

	ret = regmap_read(map, H17_PS_OFF, &word);
	if (ret) {
		dev_err(dev, "ane_h17_cpu_vote RESULT stage=3c verdict=FAIL reason=pmgr-read-ane_sys error=%d\n",
			ret);
		goto out;
	}
	h17_print_word(dev, "ane_sys", H17_PS_OFF, word);

	for (i = 0; i < nsub; i++) {
		ret = regmap_read(map, h17_sub_off[i], &word);
		if (ret) {
			dev_err(dev, "ane_h17_cpu_vote RESULT stage=3c verdict=FAIL reason=pmgr-read-%s error=%d\n",
				h17_sub_name[i], ret);
			goto out;
		}
		h17_print_word(dev, h17_sub_name[i], h17_sub_off[i], word);
		if (FIELD_GET(H17_PS_ACTUAL, word) != 0xf)
			all_on = false;
	}

	dev_info(dev, "ane_h17_cpu_vote RESULT stage=3c verdict=PASS reason=%s\n",
		 all_on ? "all-subdomains-on" : "read-ok-some-off");
	ret = 0;
out:
	for (i = nsub; i-- > 0; ) {
		if (link[i])
			device_link_del(link[i]);
		if (pd[i])
			dev_pm_domain_detach(pd[i], true);
	}
	return ret;
}

static const struct of_device_id ane_h17_cpu_vote_of_match[] = {
	{ .compatible = H17_VOTE_NODE },
	{}
};
MODULE_DEVICE_TABLE(of, ane_h17_cpu_vote_of_match);

static struct platform_driver ane_h17_cpu_vote_driver = {
	.probe = ane_h17_cpu_vote_probe,
	.driver = {
		.name = "ane_h17_cpu_vote_probe",
		.of_match_table = ane_h17_cpu_vote_of_match,
	},
};

static int __init ane_h17_cpu_vote_init(void)
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
	return platform_driver_register(&ane_h17_cpu_vote_driver);
refused:
	pr_err("ane_h17_cpu_vote RESULT stage=3c verdict=REFUSED reason=board-optin-or-dt error=%d\n", ret);
	return ret;
}

static void __exit ane_h17_cpu_vote_exit(void)
{
	platform_driver_unregister(&ane_h17_cpu_vote_driver);
}

module_init(ane_h17_cpu_vote_init);
module_exit(ane_h17_cpu_vote_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J700 H17 ANE sub-domain genpd vote; no ANE MMIO");
