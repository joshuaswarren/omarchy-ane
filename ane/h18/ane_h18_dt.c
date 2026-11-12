// SPDX-License-Identifier: GPL-2.0-only OR MIT
#ifndef ANE_H18_DT_TEST
#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#endif
#include "ane_h18_dt.h"
#include "ane_h18_dt_t8132_data.h"

static DEFINE_MUTEX(dt_lock);
static const char *dt_optin, *dt_stage;
static struct dentry *dt_control;
static int dt_id;
static bool dt_pinned, dt_applying;

static bool ane_h18_dt_stage_allowed(const char *stage)
{
	/* the t8132 diagnostic row is incomplete: only the overlay apply (dt) and the
	 * read-only register probe (reads) exist. psd would fight the PMP-managed plane,
	 * and power/boot need windows the row does not describe. */
	return stage && (!strcmp(stage, "dt") || !strcmp(stage, "reads"));
}

/* The t8132 overlay's power-domains cells are pinned to raw phandles: the live FDT carries
 * no __symbols__, so the overlay references the three ANE pmgr pwrstate nodes numerically
 * (ps_ane_mpm, ps_ane_tdbase, ps_ane_cpu; the packaging overlay pins the explicit phandles
 * in t6050-pmgr.dtsi; sim/test_dt_pmp checks the blob cells equal these constants).
 * ps_ane_cpu brings ps_ane_sys up as its parent. There is no PMP report cell: Aurora's
 * T6050 ANE node powers the engine through the pmgr domains alone, and the PMP firmware
 * took an unhandled SError on the REQ-bit write in both orders tried (2026-11-11).
 * In dtbs without the pins these numbers are unrelated nodes (an iommu, a PCIe
 * controller), so a foreign or dangling claim must refuse before anything is pinned or
 * applied: each phandle must resolve to a pmgr pwrstate node whose reg is the ANE
 * register offset in pmgr1. A base-dtb rebuild can shift the pins - re-derive them from
 * the installed base dtb before a rerun, and update these defines with them. */
#define ANE_H18_DT_T8132_PS_MPM_PHANDLE 0xa2u
#define ANE_H18_DT_T8132_PS_TDBASE_PHANDLE 0xa3u
#define ANE_H18_DT_T8132_PS_CPU_PHANDLE 0xa4u

static bool ane_h18_pwrstate_node_ok(struct device_node *np, u32 reg_want)
{
	u32 reg;

	return np && (of_device_is_compatible(np, "apple,t6050-pmgr-pwrstate") ||
		      of_device_is_compatible(np, "apple,t8103-pmgr-pwrstate")) &&
	       of_property_read_u32(np, "reg", &reg) == 0 && reg == reg_want;
}

/* The driver core powers a provider-carrying node at bind time, BEFORE probe can check
 * anything, so every claim is verified while the change is still unapplied. A shifted or
 * dangling phandle refuses the apply. Required, not advisory: the t8132 blob always
 * carries the providers, so a missing or foreign target fails closed. */
static int ane_h18_ps_targets_ok(void)
{
	static const struct {
		u32 phandle;
		u32 reg;
		const char *what;
	} claims[] = {
		{ ANE_H18_DT_T8132_PS_MPM_PHANDLE, 0x3c0, "ps_ane_mpm" },
		{ ANE_H18_DT_T8132_PS_TDBASE_PHANDLE, 0x3d0, "ps_ane_tdbase" },
		{ ANE_H18_DT_T8132_PS_CPU_PHANDLE, 0x3c8, "ps_ane_cpu" },
	};
	size_t i;

	for (i = 0; i < sizeof(claims) / sizeof(claims[0]); i++) {
		struct device_node *np = of_find_node_by_phandle(claims[i].phandle);
		bool ok = ane_h18_pwrstate_node_ok(np, claims[i].reg);

		of_node_put(np);
		if (ok)
			continue;
		pr_err("ane_h18 dt: power-domains cell %zu (phandle %#x) does not resolve to %s (pmgr pwrstate, reg %#x) in the live tree - the final dtb predates or shifted the claim; re-derive all claimed phandles from the installed base dtb\n",
		       i, claims[i].phandle, claims[i].what, claims[i].reg);
		return -EINVAL;
	}
	return 0;
}

static int ane_h18_dt_absent(void)
{
	struct device_node *np;

	/* the diagnostic overlay creates the ane node; refuse when the live tree already
	 * carries one (a second node would attach a second voter) */
	np = of_find_compatible_node(NULL, NULL, "apple,t8132-ane");
	if (!np)
		np = of_find_node_by_path("/soc/ane@5080000000");
	if (!np)
		return 0;
	of_node_put(np);
	return -EEXIST;
}

static int ane_h18_dt_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	struct of_overlay_notify_data *nd = data;
	struct device_node *soc;
	const char *key;
	int ret;

	(void)nb;
	if (action != OF_OVERLAY_PRE_APPLY || !dt_applying)
		return NOTIFY_DONE;
	if (of_property_read_string(nd->overlay->parent->parent, "omarchy,opt-in", &key))
		return NOTIFY_DONE;
	/* the diagnostic key is the only key this module accepts: a foreign or stale
	 * overlay (a wrong key on a reused address) refuses the apply outright */
	if (strcmp(key, "ane-t8132-diagnostic"))
		return notifier_from_errno(-EPERM);
	soc = of_find_node_by_path("/soc");
	ret = soc && nd->target == soc ? ane_h18_dt_absent() : -EPERM;
	of_node_put(soc);
	return notifier_from_errno(ret);
}

static struct notifier_block dt_notifier = { .notifier_call = ane_h18_dt_notify };

static int ane_h18_dt_remove(void)
{
	int ret;

	if (!dt_pinned)
		return -ENOENT;
	ret = of_overlay_remove(&dt_id);
	if (!ret && !dt_id) {
		dt_pinned = false;
		module_put(THIS_MODULE);
	} else {
		if (!ret)
			ret = -EIO;
		pr_err("ane_h18 DT removal retained ownership: id=%d error=%d\n", dt_id, ret);
	}
	return ret;
}

static int ane_h18_dt_command(const char *command)
{
	int ret;

	if (THIS_MODULE->state != MODULE_STATE_LIVE)
		return -ENODEV;
	if (!strcmp(command, "remove"))
		return ane_h18_dt_remove();
	if (strcmp(command, "apply"))
		return -EINVAL;
	if (dt_pinned)
		return -EALREADY;
	if (!dt_optin || strcmp(dt_optin, "t8132") || !ane_h18_dt_stage_allowed(dt_stage))
		return -EPERM;
	if (!of_machine_is_compatible("apple,k114c") || !of_machine_is_compatible("apple,t6050"))
		return -ENODEV;
	/* before anything is pinned or applied: a shifted or dangling provider phandle
	 * must refuse while the change is still bytes. The blob always carries the
	 * providers, so the check is required and fails closed. */
	ret = ane_h18_ps_targets_ok();
	if (ret)
		return ret;
	__module_get(THIS_MODULE);
	dt_pinned = true;
	dt_applying = true;
	ret = of_overlay_fdt_apply(ane_h18_dt_t8132_blob, sizeof(ane_h18_dt_t8132_blob), &dt_id, NULL);
	dt_applying = false;
	if (ret) {
		if (dt_id)
			ane_h18_dt_remove();
		else {
			dt_pinned = false;
			module_put(THIS_MODULE);
		}
		return ret;
	}
	return dt_id ? 0 : -EIO;
}

static ssize_t ane_h18_dt_write(struct file *file, const char __user *buffer,
			       size_t count, loff_t *position)
{
	char command[8];
	size_t size = count;
	int ret;

	(void)file;
	(void)position;
	if (!size || size >= sizeof(command))
		return -EINVAL;
	if (copy_from_user(command, buffer, size))
		return -EFAULT;
	if (memchr(command, '\0', size))
		return -EINVAL;
	if (command[size - 1] == '\n')
		--size;
	command[size] = '\0';
	mutex_lock(&dt_lock);
	ret = ane_h18_dt_command(command);
	mutex_unlock(&dt_lock);
	if (ret)
		return ret;
	return count;
}

static const struct file_operations dt_fops = {
	.owner = THIS_MODULE,
	.write = ane_h18_dt_write,
};

int ane_h18_dt_register(struct platform_driver *driver, const char *optin, const char *stage)
{
	/* the control exists only for the t8132 diagnostic opt-in on a t6050 machine with
	 * no live ane node; every other load registers the driver exactly as the plain
	 * module_platform_driver macro would */
	bool diagnostic = optin && !strcmp(optin, "t8132") && ane_h18_dt_stage_allowed(stage);
	int ret;

	if (diagnostic) {
		if (!of_machine_is_compatible("apple,k114c") || !of_machine_is_compatible("apple,t6050"))
			return -ENODEV;
		ret = ane_h18_dt_absent();
		if (ret)
			return ret;
	}
	ret = platform_driver_register(driver);
	if (ret || !diagnostic)
		return ret;
	dt_optin = optin;
	dt_stage = stage;
	ret = of_overlay_notifier_register(&dt_notifier);
	if (ret)
		goto unregister;
	dt_control = debugfs_create_file("ane_h18_dt", 0200, NULL, NULL, &dt_fops);
	if (IS_ERR_OR_NULL(dt_control)) {
		ret = dt_control ? PTR_ERR(dt_control) : -ENOMEM;
		dt_control = NULL;
		of_overlay_notifier_unregister(&dt_notifier);
		goto unregister;
	}
	return 0;
unregister:
	platform_driver_unregister(driver);
	return ret;
}

void ane_h18_dt_unregister(struct platform_driver *driver, const char *optin, const char *stage)
{
	(void)optin;
	(void)stage;
	if (dt_control) {
		debugfs_remove(dt_control);
		of_overlay_notifier_unregister(&dt_notifier);
		dt_control = NULL;
	}
	platform_driver_unregister(driver);
}
