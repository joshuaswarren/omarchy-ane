/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* sim/shim.h — userspace shim so the REAL ane_h15 module source
 * (ane_h15_main.c + ane_h15_soc.c) compiles and runs unmodified in the
 * harness. Pattern and much of the shape: the PMP vehicle harness
 * (vehicles/jwm1-pmp-oot/sim/shim.h). Provides a fake MMIO bus with a
 * full read/write event record, fake DT node, fake runtime-PM counters,
 * and a capture log for every dev_* line, so the harness can assert on
 * RESULT lines, word logs, and MMIO silence.
 *
 * A sim is not silicon: this proves control flow and gate logic, never
 * hardware behavior.
 */
#ifndef SIM_SHIM_H
#define SIM_SHIM_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <errno.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;

#define GENMASK(h, l) \
	(((~0UL) << (l)) & (~0UL >> (BITS_PER_LONG - 1 - (h))))
#define BITS_PER_LONG 64
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define FIELD_GET(m, v) (((v) & (m)) >> (__builtin_ctzll(m)))
#define FIELD_PREP(m, v) ((((u64)(v)) << (__builtin_ctzll(m))) & (m))

/* ---- annotations ---- */
#define __iomem
#define __init
#ifndef __always_inline
#define __always_inline inline
#endif

/* ---- capture log: every dev_* line lands here (and on stdout) ---- */
#define SIM_LOG_SZ (1 << 16)
extern char sim_log[SIM_LOG_SZ];
extern size_t sim_log_len;

static inline void sim_log_reset(void)
{
	sim_log_len = 0;
	sim_log[0] = '\0';
}

static inline void rec_vlog(const char *fmt, va_list ap)
{
	va_list ap2;
	int n;

	if (sim_log_len + 1 < SIM_LOG_SZ) {
		va_copy(ap2, ap);
		n = vsnprintf(sim_log + sim_log_len, SIM_LOG_SZ - sim_log_len,
			      fmt, ap2);
		va_end(ap2);
		if (n > 0)
			sim_log_len += (size_t)n;
	}
	vprintf(fmt, ap);
}

static inline void rec_log(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	rec_vlog(fmt, ap);
	va_end(ap);
}

#define pr_info(...) rec_log(__VA_ARGS__)
#define pr_err(...) rec_log("ERR: " __VA_ARGS__)
#define pr_warn(...) rec_log("WARN: " __VA_ARGS__)
#define pr_crit(...) rec_log("CRIT: " __VA_ARGS__)
#define pr_emerg(...) rec_log("EMERG: " __VA_ARGS__)
#define printk pr_info

/* ---- fake DT node + device ---- */
struct device_node {
	const char *compatible;
	const void *match_data;	/* set by the harness from the REAL table */
	bool has_ane_type;
	u32 ane_type;
	int n_reg;		/* valid reg entries: 0, 1 (truncated), 2 */
	u64 reg_pa[2];
	u64 reg_size[2];
};

struct device {
	struct device_node *of_node;
	const char *name;
};

struct platform_device {
	struct device dev;
	void *drvdata;
};

#define dev_info(d, ...)  do { (void)(d); rec_log(__VA_ARGS__); } while (0)
#define dev_err(d, ...)   do { (void)(d); rec_log("ERR: " __VA_ARGS__); } while (0)
#define dev_warn(d, ...)  do { (void)(d); rec_log("WARN: " __VA_ARGS__); } while (0)
#define dev_crit(d, ...)  do { (void)(d); rec_log("CRIT: " __VA_ARGS__); } while (0)
#define dev_emerg(d, ...) do { (void)(d); rec_log("EMERG: " __VA_ARGS__); } while (0)

static inline int of_property_read_u32(const struct device_node *n,
				       const char *name, u32 *v)
{
	if (n && n->has_ane_type && !strcmp(name, "apple,ane-type")) {
		*v = n->ane_type;
		return 0;
	}
	return -EINVAL;
}

static inline const void *of_device_get_match_data(struct device *dev)
{
	return dev->of_node ? dev->of_node->match_data : NULL;
}

struct of_device_id {
	const char *compatible;
	const void *data;
};

struct driver_shim {
	const char *name;
	bool suppress_bind_attrs;
	const struct of_device_id *of_match_table;
};

struct platform_driver {
	struct driver_shim driver;
	int (*probe)(struct platform_device *pdev);
	void (*remove)(struct platform_device *pdev);
};

#define module_param(n, t, p)
#define MODULE_PARM_DESC(n, d)
#define MODULE_AUTHOR(x)
#define MODULE_LICENSE(x)
#define MODULE_DESCRIPTION(x)
#define MODULE_DEVICE_TABLE(t, x)
#define module_platform_driver(x) extern char __sim_kmod_noop

/* ---- fake MMIO bus with a full access record ---- */
#define SIM_MAX_REGIONS 8
#define SIM_MAX_EVENTS 4096
struct sim_region {
	u64 pa;
	u64 size;
	u32 *mem;
};
struct sim_event {
	int write;
	u64 pa;
	u32 val;
};
extern struct sim_region sim_bus[SIM_MAX_REGIONS];
extern int sim_n_regions;
extern struct sim_event sim_events[SIM_MAX_EVENTS];
extern int sim_n_events;
extern int sim_bus_faults;

static inline struct sim_region *sim_region_find(u64 pa)
{
	int i;

	for (i = 0; i < sim_n_regions; i++)
		if (pa >= sim_bus[i].pa && pa < sim_bus[i].pa + sim_bus[i].size)
			return &sim_bus[i];
	return NULL;
}

/* Create-or-find; a new region is zeroed. of_iomap calls this. */
static inline void *sim_bus_map(u64 pa, u64 size)
{
	struct sim_region *r = sim_region_find(pa);

	if (r)
		return (void *)(uintptr_t)pa;
	if (sim_n_regions >= SIM_MAX_REGIONS) {
		fprintf(stderr, "SIM: region table full\n");
		exit(2);
	}
	r = &sim_bus[sim_n_regions++];
	r->pa = pa;
	r->size = size;
	r->mem = calloc(size / 4 + 1, 4);
	if (!r->mem) {
		fprintf(stderr, "SIM: out of memory for bus region\n");
		exit(2);
	}
	return (void *)(uintptr_t)pa;
}

static inline void sim_event(int write, u64 pa, u32 val)
{
	if (sim_n_events < SIM_MAX_EVENTS) {
		sim_events[sim_n_events].write = write;
		sim_events[sim_n_events].pa = pa;
		sim_events[sim_n_events].val = val;
	}
	sim_n_events++;
}

static inline u32 bus_read(u64 pa)
{
	struct sim_region *r = sim_region_find(pa);

	if (!r) {
		sim_bus_faults++;
		sim_event(0, pa, 0);
		return 0xdeadbeef;
	}
	sim_event(0, pa, r->mem[(pa - r->pa) >> 2]);
	return r->mem[(pa - r->pa) >> 2];
}

static inline void bus_write(u64 pa, u32 v)
{
	struct sim_region *r = sim_region_find(pa);

	if (!r) {
		sim_bus_faults++;
		sim_event(1, pa, v);
		return;
	}
	sim_event(1, pa, v);
	r->mem[(pa - r->pa) >> 2] = v;
}

#define ioremap_np(pa, size) sim_bus_map((pa), (size))
#define ioremap(pa, size) sim_bus_map((pa), (size))
#define iounmap(p) ((void)(p))
#define of_iomap(n, idx) \
	(!(n) || (idx) < 0 || (idx) >= (n)->n_reg || !(n)->reg_size[idx] \
		 ? NULL \
		 : sim_bus_map((n)->reg_pa[idx], (n)->reg_size[idx]))
#define readl(a) bus_read((u64)(uintptr_t)(a))
#define readl_relaxed(a) bus_read((u64)(uintptr_t)(a))
#define writel(v, a) bus_write((u64)(uintptr_t)(a), (v))

/* ---- virtual clock: 1 jiffy = 1 ms; usleep advances it so the pmgr
 * wait loop terminates against a word that never turns on ----
 */
extern unsigned long sim_jiffies;
#define jiffies sim_jiffies
static inline unsigned long msecs_to_jiffies(unsigned int ms) { return ms; }
static inline bool time_after(unsigned long a, unsigned long b)
{
	return (long)(b - a) < 0;
}
static inline void usleep_range(unsigned long lo, unsigned long hi)
{
	sim_jiffies += (lo + hi) / 2;
}
static inline void msleep(unsigned int ms) { sim_jiffies += ms; }

/* ---- runtime PM counters: the harness asserts get==put (power
 * claim released on a clean exit) ----
 */
extern int sim_pm_get;
extern int sim_pm_put;
static inline void pm_runtime_enable(struct device *d) { (void)d; }
static inline void pm_runtime_disable(struct device *d) { (void)d; }
static inline int pm_runtime_get_sync(struct device *d)
{
	(void)d;
	return ++sim_pm_get;
}
static inline int pm_runtime_put_sync(struct device *d)
{
	(void)d;
	return ++sim_pm_put;
}

/* ---- allocators / drvdata ---- */
#define GFP_KERNEL 0
static inline void *devm_kzalloc(struct device *d, size_t s, int gfp)
{
	(void)d;
	(void)gfp;
	return calloc(1, s);
}
static inline void platform_set_drvdata(struct platform_device *p, void *v)
{
	p->drvdata = v;
}
static inline void *platform_get_drvdata(struct platform_device *p)
{
	return p->drvdata;
}

#endif /* SIM_SHIM_H */
