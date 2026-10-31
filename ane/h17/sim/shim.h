/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef SIM_SHIM_H
#define SIM_SHIM_H
#include <errno.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#define SIM_LOG_SZ 4096
extern char sim_log[SIM_LOG_SZ];
extern size_t sim_log_len;
extern bool sim_j700;
static inline void rec_log(const char *fmt, ...)
{
	va_list ap;
	int n;
	size_t left = SIM_LOG_SZ - sim_log_len;
	if (left <= 1)
		return;
	va_start(ap, fmt);
	n = vsnprintf(sim_log + sim_log_len, left, fmt, ap);
	va_end(ap);
	if (n > 0)
		sim_log_len += (size_t)n < left ? (size_t)n : left - 1;
}
static inline bool of_machine_is_compatible(const char *name)
{
	return sim_j700 && !strcmp(name, "apple,j700");
}
/* No MMIO, watchdog, registration, file or PMGR stubs: unsafe new
 * dependencies in the refusal-only module must cause a build failure. */
#define pr_err(...) rec_log(__VA_ARGS__)
#define module_param(name, type, perm)
#define MODULE_PARM_DESC(name, desc)
#define __init
#define __exit
#define module_init(fn) static int (*sim_init_fn)(void) = fn
#define module_exit(fn) static void (*sim_exit_fn)(void) = fn
#define MODULE_LICENSE(x)
#define MODULE_DESCRIPTION(x)
#endif
