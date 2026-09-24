// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane_pdraise.c — raise named Apple pmgr power domains through genpd.
 *
 * For each label in `domains` (comma separated, raised in that order) the
 * module finds the apple,pmgr-pwrstate node with that label, attaches a
 * dummy platform device to it with of_genpd_add_device(), and takes a
 * runtime-PM reference. genpd powers the domain and its parents; the
 * pmgr-pwrstate driver does the register writes. No raw pmgr access.
 * Each raise is logged before it happens, so a reset pins the domain.
 * Unloading drops the references in reverse order.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/string.h>

#define MAX_PD 16

static char *domains = "venc_sys,venc_dma,venc_pipe4,venc_pipe5,venc_me0";
module_param(domains, charp, 0444);
MODULE_PARM_DESC(domains, "pmgr pwrstate labels to raise, in order");

static struct {
	struct platform_device *pdev;
	bool attached;
	bool powered;
	char label[32];
} pd[MAX_PD];
static int npd;

static struct device_node *find_pd(const char *label)
{
	struct device_node *np;
	const char *l;

	for_each_compatible_node(np, NULL, "apple,pmgr-pwrstate")
		if (!of_property_read_string(np, "label", &l) && !strcmp(l, label))
			return np;
	return NULL;
}

static void ane_pdraise_cleanup(void)
{
	while (npd--) {
		struct device *dev = &pd[npd].pdev->dev;

		if (pd[npd].powered) {
			pr_info("ane_pdraise: releasing %s\n", pd[npd].label);
			pm_runtime_put_sync(dev);
		}
		if (pd[npd].attached) {
			pm_runtime_disable(dev);
			pm_genpd_remove_device(dev);
		}
		platform_device_unregister(pd[npd].pdev);
	}
	npd = 0;
}

static int __init ane_pdraise_init(void)
{
	char *buf, *s, *tok;
	int ret = 0;

	buf = kstrdup(domains, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	s = buf;
	while ((tok = strsep(&s, ","))) {
		struct of_phandle_args spec = { 0 };
		struct device *dev;

		if (!*tok)
			continue;
		if (npd == MAX_PD) {
			ret = -E2BIG;
			break;
		}
		spec.np = find_pd(tok);
		if (!spec.np) {
			pr_err("ane_pdraise: no pwrstate labelled %s\n", tok);
			ret = -ENODEV;
			break;
		}
		pd[npd].pdev = platform_device_register_simple("ane_pdraise",
							       npd, NULL, 0);
		if (IS_ERR(pd[npd].pdev)) {
			ret = PTR_ERR(pd[npd].pdev);
			of_node_put(spec.np);
			break;
		}
		strscpy(pd[npd].label, tok, sizeof(pd[npd].label));
		dev = &pd[npd].pdev->dev;
		npd++;

		ret = of_genpd_add_device(&spec, dev);
		of_node_put(spec.np);
		if (ret) {
			pr_err("ane_pdraise: attach %s: %pe\n", tok, ERR_PTR(ret));
			break;
		}
		pd[npd - 1].attached = true;
		pm_runtime_enable(dev);

		pr_info("ane_pdraise: raising %s\n", tok);
		ret = pm_runtime_resume_and_get(dev);
		if (ret) {
			pr_err("ane_pdraise: raise %s: %pe\n", tok, ERR_PTR(ret));
			break;
		}
		pd[npd - 1].powered = true;
		pr_info("ane_pdraise: %s on\n", tok);
	}
	kfree(buf);
	if (ret)
		ane_pdraise_cleanup();
	return ret;
}

static void __exit ane_pdraise_exit(void)
{
	ane_pdraise_cleanup();
}

module_init(ane_pdraise_init);
module_exit(ane_pdraise_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Raise named Apple pmgr power domains through genpd");
