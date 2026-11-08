// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * J700 stage 4 is not qualified. In particular, no source-backed wake /
 * quiesce protocol or watchdog ownership adapter exists. Do not retain
 * speculative register writes behind a single qualification switch.
 * The reviewed draft is preserved at 9f24a62160c95d34935e3837feae325284ced7eb.
 * transaction.h supplies tested read-only transaction logic for future
 * qualified adapters; it is intentionally not connected to hardware here.
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include "transaction.h"

static bool optin;
module_param(optin, bool, 0444);
MODULE_PARM_DESC(optin, "Historical opt-in; execution remains unavailable");

static bool contain_rehearse;
module_param(contain_rehearse, bool, 0444);
MODULE_PARM_DESC(contain_rehearse, "Historical parameter; watchdog rehearsals remain unavailable");

static int __init ane_h17_wake_init(void)
{
	if (!optin || !of_machine_is_compatible("apple,j700")) {
		pr_err("ane_h17_wake RESULT stage=4 verdict=REFUSED reason=board-optin-or-dt\n");
		return -EPERM;
	}
	pr_err("ane_h17_wake RESULT stage=4 verdict=REFUSED reason=unqualified-register-map; wake and recovery adapters unavailable\n");
	return -EOPNOTSUPP;
}

static void __exit ane_h17_wake_exit(void)
{
}

module_init(ane_h17_wake_init);
module_exit(ane_h17_wake_exit);
MODULE_LICENSE("Dual MIT/GPL");
MODULE_DESCRIPTION("J700 H17 stage 4 unavailable pending protocol qualification");
