/* Host shim for <linux/module.h> — only THIS_MODULE. */
#ifndef ANE_STATS_SHIM_LINUX_MODULE_H
#define ANE_STATS_SHIM_LINUX_MODULE_H

struct module {
	int unused;
};

#define THIS_MODULE ((struct module *)0)

#endif /* ANE_STATS_SHIM_LINUX_MODULE_H */
