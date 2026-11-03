/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * ane_h16_overlay.h: run-time device-tree change for ane_h16 (opt-in, off by default).
 *
 * On a T6040 whose device tree already holds the ANE node, its five power controllers and its DART nodes but marks them
 * disabled, apply_overlay=1 enables the five power controllers and the ANE node and completes the ANE node (the pmgr window,
 * reg-names, apple,ane-type and the full power-domains list), all in one change set. The DART nodes are not touched. Unloading
 * the module reverts the change set. Enabled nodes can trigger provider and power activity.
 */
#ifndef __ANE_H16_OVERLAY_H__
#define __ANE_H16_OVERLAY_H__

#include <linux/types.h>

struct platform_driver;

/* Apply the change when apply_overlay is set, then register the driver. Fails closed: a refused guard returns an error and the
 * device tree is untouched. */
int ane_h16_register(struct platform_driver *drv, const char *optin, bool apply_overlay);

/* Unregister the driver, then revert what ane_h16_register applied (nothing if it applied nothing). */
void ane_h16_unregister(struct platform_driver *drv);

#endif
