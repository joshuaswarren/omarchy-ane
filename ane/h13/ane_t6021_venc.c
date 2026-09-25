// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane_t6021_venc.c — raise the T6021 ANE VENC rails through genpd.
 *
 * Same shape as ane_pdraise.c: for each label in `domains` (comma
 * separated, raised in that order) the module finds the
 * apple,pmgr-pwrstate node with that label, attaches a dummy platform
 * device to it with of_genpd_add_device(), and takes a runtime-PM
 * reference. genpd powers the domain and its parents; the
 * pmgr-pwrstate driver does the register writes. No raw pmgr access.
 * Each raise is logged before it happens, so a reset pins the domain.
 * Unloading drops the references in reverse order.
 *
 * T6021-only: refuses to load unless an apple,t6021-ane node is present
 * in the device tree, so M1 (t8103) and M1 Max (t6001) paths are
 * untouched. Default order is the kext's parents-first sequence that
 * the 13.5 AppleH11ANEInterface EnableANEClocksAndPower uses on the
 * pre-ANE_Init path: VENC_SYS, VENC_DMA, then the PIPE4/PIPE5/ME0
 * leaves. The firmware runs code only with this leg up (DATA footprint
 * in receipts/2026-09-24-t6021-pwgate); without it the ASC parks before
 * its first store.
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

static void ane_t6021_venc_cleanup(void)
{
	while (npd--) {
		struct device *dev = &pd[npd].pdev->dev;

		if (pd[npd].powered) {
			pr_info("ane_t6021_venc: releasing %s\n", pd[npd].label);
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

static int __init ane_t6021_venc_init(void)
{
	struct device_node *ane;
	char *buf, *s, *tok;
	int ret = 0;

	/* T6021-only gate: no apple,t6021-ane node, no load. */
	ane = of_find_compatible_node(NULL, NULL, "apple,t6021-ane");
	if (!ane) {
		pr_err("ane_t6021_venc: no apple,t6021-ane node; refusing\n");
		return -ENODEV;
	}
	of_node_put(ane);

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
			pr_err("ane_t6021_venc: no pwrstate labelled %s\n", tok);
			ret = -ENODEV;
			break;
		}
		pd[npd].pdev = platform_device_register_simple("ane_t6021_venc",
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
			pr_err("ane_t6021_venc: attach %s: %pe\n", tok, ERR_PTR(ret));
			break;
		}
		pd[npd - 1].attached = true;
		pm_runtime_enable(dev);

		pr_info("ane_t6021_venc: raising %s\n", tok);
		ret = pm_runtime_resume_and_get(dev);
		if (ret) {
			pr_err("ane_t6021_venc: raise %s: %pe\n", tok, ERR_PTR(ret));
			break;
		}
		pd[npd - 1].powered = true;
		pr_info("ane_t6021_venc: %s on\n", tok);
	}
	kfree(buf);
	if (ret)
		ane_t6021_venc_cleanup();
	return ret;
}

static void __exit ane_t6021_venc_exit(void)
{
	ane_t6021_venc_cleanup();
}

module_init(ane_t6021_venc_init);
module_exit(ane_t6021_venc_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("Raise the T6021 ANE VENC rails through genpd (parents first)");
