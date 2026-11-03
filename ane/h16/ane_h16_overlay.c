// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane_h16_overlay.c: see ane_h16_overlay.h. The values come from the T6040 device tree (t6040.dtsi, t6040-pmgr.dtsi) and the t6040 row
 * of data/ane-soc: engine window 0x484000000 + 0x2000000, pmgr window 0x502280000 + 0x18000, ane-type 0x110. The nodes sit at the
 * root of the tree, not under /soc.
 */
#include <linux/errno.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/types.h>

#include "ane_h16_overlay.h"

#define OVL "ane_h16 overlay: "
#define ANE_PATH "/ane@484000000"
#define N_PS 5

/* sys, mpm, cpu, td, base: the order of the pmgr words in the t6040 row and of the power-domains list. */
static const char *const ps_path[N_PS] = {
	"/power-management@502280000/power-controller@3c0",
	"/power-management@50228c000/power-controller@0",
	"/power-management@50228c000/power-controller@8",
	"/power-management@50228c000/power-controller@10",
	"/power-management@50228c000/power-controller@18",
};

/* The nodes the power controllers hang under and the parents of ps_ane_sys: all must be available, or the domains cannot come up. */
static const char *const needs_path[] = {
	"/power-management@502280000",
	"/power-management@50228c000",
	"/power-management@502280000/power-controller@128",
	"/power-management@502800000/power-controller@440",
};

static const char *const dart_path[] = { "/iommu@485800000", "/iommu@485820000", "/iommu@485840000" };

static __be32 reg_val[8] = {
	cpu_to_be32(0x4), cpu_to_be32(0x84000000), cpu_to_be32(0x0), cpu_to_be32(0x2000000),
	cpu_to_be32(0x5), cpu_to_be32(0x02280000), cpu_to_be32(0x0), cpu_to_be32(0x18000),
};
static char reg_names_val[] = "engine\0pmgr";
static __be32 pd_val[N_PS];
static char okay_val[] = "okay";

static struct property p_reg = { .name = "reg", .length = sizeof(reg_val), .value = reg_val };
static struct property p_reg_names = { .name = "reg-names", .length = sizeof(reg_names_val), .value = reg_names_val };
static struct property p_pd = { .name = "power-domains", .length = sizeof(pd_val), .value = pd_val };
static struct property p_status = { .name = "status", .length = sizeof(okay_val), .value = okay_val };

static struct {
	struct of_changeset cs;
	bool applied;
} ane_ovl;

static phandle ane_h16_max_phandle(void)
{
	struct device_node *np;
	phandle max = 0;

	for (np = of_find_all_nodes(NULL); np; np = of_find_all_nodes(np))
		if (np->phandle > max)
			max = np->phandle;
	return max;
}

/* Minted phandles remain after removal because live-tree references may retain them. */
static void ane_h16_assign_phandles(struct device_node **ps)
{
	phandle next = ane_h16_max_phandle() + 1;
	unsigned int i;

	for (i = 0; i < N_PS; i++) {
		if (ps[i]->phandle)
			continue;
		ps[i]->phandle = next++;
		pr_info(OVL "gave %s phandle %u\n", ps_path[i], ps[i]->phandle);
	}
}

/* Every guard that can refuse runs here, before any change. */
static int ane_h16_check(struct device_node *ane, struct device_node **ps)
{
	unsigned int i;

	if (!of_machine_is_compatible("apple,t6040")) {
		pr_err(OVL "refused: this machine is not an apple,t6040\n");
		return -ENODEV;
	}
	if (!ane) {
		pr_err(OVL "refused: %s is not in the device tree\n", ANE_PATH);
		return -ENOENT;
	}
	if (!of_device_is_compatible(ane, "apple,t6041-ane")) {
		pr_err(OVL "refused: %s is not apple,t6041-ane\n", ANE_PATH);
		return -ENODEV;
	}
	for (i = 0; i < N_PS; i++) {
		if (!ps[i]) {
			pr_err(OVL "refused: %s is not in the device tree\n", ps_path[i]);
			return -ENOENT;
		}
	}
	for (i = 0; i < ARRAY_SIZE(needs_path); i++) {
		struct device_node *np = of_find_node_by_path(needs_path[i]);
		bool ok = of_device_is_available(np);

		of_node_put(np);
		if (!ok) {
			pr_err(OVL "refused: %s is not available\n", needs_path[i]);
			return np ? -ENODEV : -ENOENT;
		}
	}
	return 0;
}

static bool ane_h16_darts_unavailable(void)
{
	unsigned int i;
	bool missing = false;

	for (i = 0; i < ARRAY_SIZE(dart_path); i++) {
		struct device_node *np = of_find_node_by_path(dart_path[i]);

		missing |= !of_device_is_available(np);
		of_node_put(np);
	}
	return missing;
}

/* One change set: the power controllers first, then the ANE node with its status last, because the platform device is created when
 * the status changes and must already see the finished node. */
static int ane_h16_build(struct device_node *ane, struct device_node **ps)
{
	struct property *iommus;
	unsigned int i;
	int ret = 0;

	of_changeset_init(&ane_ovl.cs);
	for (i = 0; i < N_PS && !ret; i++)
		if (!of_device_is_available(ps[i]))
			ret = of_changeset_update_property(&ane_ovl.cs, ps[i], &p_status);
	for (i = 0; i < N_PS; i++)
		pd_val[i] = cpu_to_be32(ps[i]->phandle);
	if (!ret)
		ret = of_changeset_update_property(&ane_ovl.cs, ane, &p_reg);
	if (!ret)
		ret = of_changeset_update_property(&ane_ovl.cs, ane, &p_reg_names);
	if (!ret)
		ret = of_changeset_update_property(&ane_ovl.cs, ane, &p_pd);
	if (!ret)
		ret = of_changeset_add_prop_u32(&ane_ovl.cs, ane, "apple,ane-type", 0x110);
	iommus = of_find_property(ane, "iommus", NULL);
	if (!ret && iommus && ane_h16_darts_unavailable())
		ret = of_changeset_remove_property(&ane_ovl.cs, ane, iommus);
	if (!ret && !of_device_is_available(ane))
		ret = of_changeset_update_property(&ane_ovl.cs, ane, &p_status);
	if (!ret)
		ret = of_changeset_apply(&ane_ovl.cs);
	if (ret)
		of_changeset_destroy(&ane_ovl.cs);
	return ret;
}

/* Returns 0 when the change was applied or the tree already has a complete node; an error, with the tree untouched, otherwise. */
static int ane_h16_overlay_apply(const char *optin)
{
	struct device_node *ps[N_PS] = { };
	struct device_node *ane;
	unsigned int i;
	int ret;

	if (!optin || strcmp(optin, "t6040")) {
		pr_err(OVL "refused: apply_overlay is for optin=t6040 only (optin=%s)\n", optin ? optin : "(unset)");
		return -EINVAL;
	}
	ane = of_find_node_by_path(ANE_PATH);
	for (i = 0; i < N_PS; i++)
		ps[i] = of_find_node_by_path(ps_path[i]);
	ret = ane_h16_check(ane, ps);
	if (!ret && of_device_is_available(ane) && of_find_property(ane, "apple,ane-type", NULL)) {
		pr_info(OVL "not applied: node already complete\n");
		goto out;
	}
	if (!ret) {
		ane_h16_assign_phandles(ps);
		ret = ane_h16_build(ane, ps);
		if (ret)
			pr_err(OVL "the kernel refused the change: %d\n", ret);
		else {
			ane_ovl.applied = true;
			pr_info(OVL "applied: ANE node and its power controllers are enabled and complete\n");
		}
	}
out:
	for (i = 0; i < N_PS; i++)
		of_node_put(ps[i]);
	of_node_put(ane);
	return ret;
}

static void ane_h16_overlay_remove(void)
{
	if (!ane_ovl.applied)
		return;
	of_changeset_revert(&ane_ovl.cs);
	of_changeset_destroy(&ane_ovl.cs);
	ane_ovl.applied = false;
}

int ane_h16_register(struct platform_driver *drv, const char *optin, bool apply_overlay)
{
	int ret = 0;

	if (apply_overlay)
		ret = ane_h16_overlay_apply(optin);
	if (ret)
		return ret;
	ret = platform_driver_register(drv);
	if (ret)
		ane_h16_overlay_remove();
	return ret;
}

void ane_h16_unregister(struct platform_driver *drv)
{
	platform_driver_unregister(drv);
	ane_h16_overlay_remove();
}
