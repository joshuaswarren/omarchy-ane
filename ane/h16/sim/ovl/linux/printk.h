/* Fake <linux/printk.h> for sim/sim_overlay.c: every message is recorded by the harness. */
#ifndef SIM_OVL_PRINTK_H
#define SIM_OVL_PRINTK_H
void sim_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define pr_info(...) sim_log(__VA_ARGS__)
#define pr_err(...) sim_log(__VA_ARGS__)
#endif
