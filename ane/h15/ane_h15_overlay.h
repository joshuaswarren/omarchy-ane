/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * ane_h15_overlay.h: run-time device-tree overlay for ane_h15 (opt-in, off by default).
 *
 * The module binds to its opt-in SoC's ANE node. On a boot whose device tree has none (a lab boot cannot carry a
 * device-tree change), apply_overlay=1 gives the module its own: the compiled per-SoC overlay is
 * applied with the kernel's overlay call, then the driver is registered and the normal probe runs.
 * Added devices can trigger provider and power activity. Unloading removes the overlay.
 */
#ifndef __ANE_H15_OVERLAY_H__
#define __ANE_H15_OVERLAY_H__

#include <linux/types.h>

struct platform_driver;

/* Apply the overlay when apply_overlay is set, then register the driver. Fails closed: a refused guard returns an error and the
 * device tree is untouched. */
int ane_h15_register(struct platform_driver *drv, const char *optin, bool apply_overlay);

/* Unregister the driver, then remove what ane_h15_register applied (nothing if it applied nothing). */
void ane_h15_unregister(struct platform_driver *drv);

#endif
