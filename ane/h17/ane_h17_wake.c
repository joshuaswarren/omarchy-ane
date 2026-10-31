// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * J700/T8140 stage 4 is quarantined. The timer/marker implementation in
 * 83a028cc0b106af0f3336f7fddfb9a81232cacf0 did not establish containment.
 * See receipts/2026-10-31-j700-h17-containment-correction/README.md.
 * Keep the experiment in Git history, not behind runtime escape hatches:
 * this module has no driver registration, power vote or MMIO path.
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>

#define H17_REGISTER_MAP_QUALIFIED 0

static bool optin;
module_param(optin, bool, 0444);
MODULE_PARM_DESC(optin, "Historical opt-in; stage 4 remains quarantined");

static int __init ane_h17_wake_init(void)
{
	if (!optin || !of_machine_is_compatible("apple,j700")) {
		pr_err("ane_h17_wake RESULT stage=4 verdict=REFUSED reason=board-optin-or-dt\n");
		return -EPERM;
	}
	if (!H17_REGISTER_MAP_QUALIFIED) {
		pr_err("ane_h17_wake RESULT stage=4 verdict=REFUSED reason=unqualified-register-map; containment unqualified; no power or MMIO access\n");
		return -EOPNOTSUPP;
	}
	return 0;
}

static void __exit ane_h17_wake_exit(void)
{
}

module_init(ane_h17_wake_init);
module_exit(ane_h17_wake_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J700 H17 stage 4 quarantined: register map and containment unqualified");
