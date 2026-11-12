/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* The t8132 diagnostic module registers its platform driver through these two
 * wrappers: for the t8132 opt-in they arm the device-tree overlay control
 * (debugfs ane_h18_dt) beside the driver; every other opt-in registers the
 * driver exactly like module_platform_driver. */
int ane_h18_dt_register(struct platform_driver *driver, const char *optin, const char *stage);
void ane_h18_dt_unregister(struct platform_driver *driver, const char *optin, const char *stage);
