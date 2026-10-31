// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * ane_h15_overlay.c: apply the compiled ANE overlay to the live device tree (apply_overlay=1), see ane_h15_overlay.h.
 *
 * Why this is more than one call. The kernel's run-time overlay code resolves a reference from the overlay to a node that is
 * already in the live tree through the live /__symbols__ table, and uses that node's own phandle. Neither is there on these
 * boots: the base trees are built without symbols, and the ANE power controllers carry a phandle only when something else
 * refers to them. The kernel also drops the phandle of an overlay node merged into an existing node, so the boot-time trick of
 * minting a label on an existing node does not work at run time. So, in this order and only after every guard holds:
 *   1. publish the few labels the overlay refers to in /__symbols__ (one changeset, reverted at unload);
 *   2. give a target node that has no phandle one (the field the kernel's lookup reads);
 *   3. apply the overlay; the kernel patches each reference with the target's phandle;
 *   4. the caller registers the driver. Devices already exist, so the normal probe runs here and not inside the overlay lock.
 * ane_h15_overlay_data.h is generated from the boot-time overlays by gen-runtime-overlay.py, which removes only the empty
 * label-minting fragments; every other node and property comes from that source, without implying hardware validation.
 */
#include <linux/errno.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/types.h>

#include "ane_h15_overlay.h"

#define OVL "ane_h15 overlay: "
#define OVL_MAX_SYMS 8

struct ane_h15_ovl_sym {
	const char *label;
	const char *path;
};

struct ane_h15_ovl {
	const char *name;		/* the optin key */
	const char *board;		/* the machine's compatible */
	const char *node;		/* the compatible of the ANE node the overlay adds */
	const u8 *blob;
	u32 size;
	const struct ane_h15_ovl_sym *syms;
	unsigned int nsyms;
};

#include "ane_h15_overlay_data.h"

static const struct ane_h15_ovl ane_h15_ovls[] = {
	{ "t8122", "apple,t8122", "apple,t8122-ane", ane_h15_ovl_t8122_blob, sizeof(ane_h15_ovl_t8122_blob),
	  ane_h15_ovl_t8122_syms, ARRAY_SIZE(ane_h15_ovl_t8122_syms) },
	{ "t6030", "apple,t6030", "apple,t6030-ane", ane_h15_ovl_t6030_blob, sizeof(ane_h15_ovl_t6030_blob),
	  ane_h15_ovl_t6030_syms, ARRAY_SIZE(ane_h15_ovl_t6030_syms) },
	{ "t6031", "apple,t6031", "apple,t6031-ane", ane_h15_ovl_t6031_blob, sizeof(ane_h15_ovl_t6031_blob),
	  ane_h15_ovl_t6031_syms, ARRAY_SIZE(ane_h15_ovl_t6031_syms) },
};

static struct {
	bool symbols;		/* the /__symbols__ changeset is applied */
	bool overlay;		/* the overlay is applied */
	int id;
	struct of_changeset cs;
} ane_ovl;

static const struct ane_h15_ovl *ane_h15_find_ovl(const char *optin)
{
	unsigned int i;

	for (i = 0; optin && i < ARRAY_SIZE(ane_h15_ovls); i++)
		if (!strcmp(optin, ane_h15_ovls[i].name))
			return &ane_h15_ovls[i];
	return NULL;
}

static phandle ane_h15_max_phandle(void)
{
	struct device_node *np;
	phandle max = 0;

	for (np = of_find_all_nodes(NULL); np; np = of_find_all_nodes(np))
		if (np->phandle > max)
			max = np->phandle;
	return max;
}

/* Every label the overlay refers to must name a node that exists, and must not already name another one. No write yet. */
static int ane_h15_check_targets(const struct ane_h15_ovl *o, struct device_node **targets)
{
	struct device_node *sym = of_find_node_by_path("/__symbols__");
	const char *cur;
	unsigned int i;
	int ret = 0;

	for (i = 0; i < o->nsyms && !ret; i++) {
		targets[i] = of_find_node_by_path(o->syms[i].path);
		if (!targets[i]) {
			pr_err(OVL "refused: %s (label %s) is not in the device tree\n", o->syms[i].path, o->syms[i].label);
			ret = -ENOENT;
		} else if (sym && !of_property_read_string(sym, o->syms[i].label, &cur) && strcmp(cur, o->syms[i].path)) {
			pr_err(OVL "refused: label %s already names %s, not %s\n", o->syms[i].label, cur, o->syms[i].path);
			ret = -EEXIST;
		}
	}
	of_node_put(sym);
	return ret;
}

/* Step 1: add each label that is not already there. */
static int ane_h15_publish_symbols(const struct ane_h15_ovl *o)
{
	struct device_node *sym = of_find_node_by_path("/__symbols__");
	bool found = sym;
	const char *cur;
	unsigned int i;
	int ret = 0;

	of_changeset_init(&ane_ovl.cs);
	if (!sym) {
		struct device_node *root = of_find_node_by_path("/");

		sym = of_changeset_create_node(&ane_ovl.cs, root, "__symbols__");
		of_node_put(root);
		if (!sym) {
			of_changeset_destroy(&ane_ovl.cs);
			return -ENOMEM;
		}
	}
	for (i = 0; i < o->nsyms && !ret; i++)
		if (of_property_read_string(sym, o->syms[i].label, &cur))
			ret = of_changeset_add_prop_string(&ane_ovl.cs, sym, o->syms[i].label, o->syms[i].path);
	if (found)
		of_node_put(sym);
	if (!ret)
		ret = of_changeset_apply(&ane_ovl.cs);
	if (ret) {
		of_changeset_destroy(&ane_ovl.cs);
		pr_err(OVL "could not publish the symbol labels: %d\n", ret);
		return ret;
	}
	ane_ovl.symbols = true;
	return 0;
}

/* Minted phandles remain after removal because live-tree references may retain them. */
static void ane_h15_assign_phandles(const struct ane_h15_ovl *o, struct device_node **targets)
{
	phandle next = ane_h15_max_phandle() + 1;
	unsigned int i;

	for (i = 0; i < o->nsyms; i++) {
		if (targets[i]->phandle)
			continue;
		targets[i]->phandle = next++;
		pr_info(OVL "gave %s (%s) phandle %u\n", o->syms[i].path, o->syms[i].label, targets[i]->phandle);
	}
}

static void ane_h15_put_targets(struct device_node **targets, unsigned int n)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		of_node_put(targets[i]);
}

/* Returns 0 when the overlay was applied or the tree already has the node; an error, with the tree untouched, otherwise. */
static int ane_h15_overlay_apply(const char *optin)
{
	const struct ane_h15_ovl *o = ane_h15_find_ovl(optin);
	struct device_node *targets[OVL_MAX_SYMS] = { };
	struct device_node *np;
	int ret;

	if (!o) {
		pr_err(OVL "refused: optin=%s has no overlay (t8122, t6030 and t6031 do)\n", optin ? optin : "(unset)");
		return -EINVAL;
	}
	if (o->nsyms > OVL_MAX_SYMS) {
		pr_err(OVL "refused: %s lists %u labels, more than the %u this code holds\n", o->name, o->nsyms, OVL_MAX_SYMS);
		return -E2BIG;
	}
	if (!of_machine_is_compatible(o->board)) {
		pr_err(OVL "refused: this machine is not an %s\n", o->board);
		return -ENODEV;
	}
	np = of_find_compatible_node(NULL, NULL, o->node);
	if (np) {
		of_node_put(np);
		pr_info(OVL "not applied: the device tree already has %s\n", o->node);
		return 0;
	}
	ret = ane_h15_check_targets(o, targets);
	if (!ret)
		ret = ane_h15_publish_symbols(o);
	if (!ret) {
		ane_h15_assign_phandles(o, targets);
		ret = of_overlay_fdt_apply(o->blob, o->size, &ane_ovl.id, NULL);
		if (ret) {
			pr_err(OVL "the kernel refused the overlay: %d\n", ret);
			if (ane_ovl.id) {
				int cleanup = of_overlay_remove(&ane_ovl.id);

				if (cleanup)
					pr_err(OVL "could not remove the failed overlay: %d\n", cleanup);
			}
			of_changeset_revert(&ane_ovl.cs);
			of_changeset_destroy(&ane_ovl.cs);
			ane_ovl.symbols = false;
		} else {
			ane_ovl.overlay = true;
			pr_info(OVL "applied %s (%u bytes, %u labels)\n", o->node, o->size, o->nsyms);
		}
	}
	ane_h15_put_targets(targets, o->nsyms);
	return ret;
}

static void ane_h15_overlay_remove(void)
{
	if (ane_ovl.overlay) {
		int ret = of_overlay_remove(&ane_ovl.id);

		if (ret)
			pr_err(OVL "could not remove the overlay: %d\n", ret);
		ane_ovl.overlay = false;
	}
	if (ane_ovl.symbols) {
		of_changeset_revert(&ane_ovl.cs);
		of_changeset_destroy(&ane_ovl.cs);
		ane_ovl.symbols = false;
	}
}

int ane_h15_register(struct platform_driver *drv, const char *optin, bool apply_overlay)
{
	int ret = 0;

	if (apply_overlay)
		ret = ane_h15_overlay_apply(optin);
	if (ret)
		return ret;
	ret = platform_driver_register(drv);
	if (ret)
		ane_h15_overlay_remove();
	return ret;
}

void ane_h15_unregister(struct platform_driver *drv)
{
	platform_driver_unregister(drv);
	ane_h15_overlay_remove();
}
